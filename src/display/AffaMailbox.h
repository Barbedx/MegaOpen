#pragma once
#include <AffaDisplay.h>
#include <Arduino.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "../app/MailboxIds.h"
#include "../utils/Locks.h"
#include "PaneBuffers.h"

// The only way for anything that is not the loop task to draw on the panel.
//
// AffaDisplay is unlocked: exactly one task (the poll() task) may call into it;
// every other context posts a request that drain() applies. Calling it from an
// HTTP handler races the transmit FSM and garbles the screen under load.
//
// A FreeRTOS queue, not AffaRing: that ring is single-producer and PsychicHttp
// serves from several tasks.
//
// Since Step 4 (docs/design/event-tasks.md) the render policy is a poster too:
// the ui task decides what goes on the glass and the panel task only pumps.
// So a post returns a ReqId, and a request posted with `track` set gets its
// verdict back as a MailboxIds::Completion (refused at enqueue, synthesised on
// the spot; or the panel's own answer, later, through libComplete()). Untracked
// posts (the web, key popups) still only reach the log.
//
// Two things the ui needs that a queue alone cannot give:
//   * the media rows are a coalescing single slot outside the queue: a newer
//     repaint replaces one not yet sent, and the stale one never costs the
//     14 frames of a three-row screen;
//   * an automatic repaint (`yields`) carries the deliberate-screen count the
//     ui had seen when it decided. If a person's screen was accepted since, the
//     repaint is dropped here (Aborted) instead of covering it: the ui cannot
//     know about that screen until the panel task has drawn it.
namespace MegaOpen
{
    class AffaMailbox {
    public:
        // Every rendering call the library offers, so the web console can
        // exercise the whole surface. No AuxMode, HideFullscreen or HideInfoPopup:
        // AffaDisplay 1.0.0 removed them (see PROTOCOL-NOTES.md §8).
        // The popup is the only overlay and the only screen with a hide; every
        // other screen is replaced by the next render (see main.cpp).
        enum class Op : uint8_t {
            Text, TextStyled, Time, Power,
            Menu, Highlight,
            Popup, HidePopup,
            Fullscreen,
            ConfirmBox, MessageBox, SelectButton,
            InfoPopup,
            Key, Abort,
            FullscreenGeom,   // posted by the ui task (UiTask.cpp), r.p = geometry
            // Not renders: what the web asks of the panel task, in order with
            // the renders around it (docs/design/event-tasks.md, Step 3). Resync
            // re-runs the handshake (begin(), which only the poll() task may
            // call); Clear hands the glass back to the media screen: it is
            // counted (clears()) and the ui task acts on the count.
            Resync, Clear,
            // The 48x48 pane from PaneBuffers slot `slot` (Step 4). The bytes
            // are borrowed until this request completes.
            NavBitmap,
        };

        using ReqId = MailboxIds::ReqId;

        static constexpr uint8_t STR_MAX = 33;   // 32 chars + NUL

        struct Req {
            Op       op = Op::Text;
            char     a[STR_MAX] = {0};
            char     b[STR_MAX] = {0};
            char     c[STR_MAX] = {0};
            uint16_t u16 = 0;      // key wire code, menu scroll byte, or row index
            bool     flag = false; // power on / key hold

            // Did a person ask for this screen? The report callback uses it to
            // hold the glass against automatic repaints. Not implied by "came
            // through the mailbox": the single-line track fallback is posted too,
            // and holding on it would starve the pane. Defaults true (safe side).
            bool     deliberate = true;
            // Per-op extra bytes: popup icon, source icon, format — exposed so
            // the console can sweep them.
            uint8_t  p[4] = {0, 0, 0, 0};

            // ---- Step 4: who waits for the answer ----------------------------
            ReqId    id    = MailboxIds::NO_REQ;     // set by post()
            // Report the verdict as a Completion (the ui task's requests).
            bool     track = false;
            // An automatic repaint: dropped if a deliberate screen was accepted
            // after `epoch` (the deliberateSeq() the poster had seen).
            bool     yields = false;
            uint16_t epoch = 0;
            // NavBitmap: the PaneBuffers slot it borrows.
            uint8_t  slot = MailboxIds::NO_SLOT;
        };

        // Reported once per drained request, from the poll() task. Nothing here
        // may call back into the display.
        using ReportFn = void (*)(const Req& r, affa::Result result, void* ctx);
        // A tracked request's verdict, on the poll() task: inside drain() for a
        // refused or dropped one, inside poll() (libComplete) for the panel's
        // answer. Post it on; never call back into the display.
        using CompletionFn = void (*)(const MailboxIds::Completion& c, void* ctx);

        // 16: a web console burst plus a key popup and a head-unit text must
        // all fit between two passes of the panel task.
        bool begin(uint8_t depth = 16);
        void onReport(ReportFn fn, void* ctx) { _report = fn; _ctx = ctx; }
        void onCompletion(CompletionFn fn, void* ctx) { _done = fn; _doneCtx = ctx; }
        // The pane's buffers: NavBitmap reads them, and a completion releases
        // the slot its request borrowed.
        void attachPane(PaneBuffers* p) { _pane = p; }

        // Carminat-only ops (TextStyled, MessageBox, SelectButton). Set by
        // createDisplay() since there is no RTTI; null -> those ops report
        // NotSupported.
        void attachCarminat(affa::CarminatDisplay* c) { _carminat = c; }

        // Any task. Copies the request and gives it an id, returned; NO_REQ (0)
        // = queue full, nothing drawn (counted in dropped()).
        ReqId post(const Req& r);

        // The media rows (Text, Menu, InfoPopup or Fullscreen, normally tracked
        // and yielding): replaces a rows request not drained yet, which is then
        // forgotten (no completion; the poster drops its own wait). Any task,
        // in practice the ui task.
        ReqId postRows(const Req& r);
        // The pane from `slot`, which the caller has lent (PaneBuffers::lend).
        // Tracked: the slot comes back with the completion. On NO_REQ it never
        // left, and the caller takes the slot back itself.
        ReqId postNavBitmap(uint8_t slot, uint16_t epoch);

        // `deliberate` false = firmware-initiated render (see Req::deliberate);
        // only the ui task's single-line fallback passes it.
        bool postText(const char* text, bool deliberate = true);
        bool postTime(const char* hhmm);
        bool postPower(bool on);
        bool postMenu(const char* header, const char* row0, const char* row1, uint8_t scroll);
        bool postHighlight(uint8_t row);
        // icon 0x55 = no icons. The OEM volume popup's 0x09 lights i(TRAFFIC),
        // AF-RDS and LIST inside the box on the bench panel.
        bool postPopup(const char* text, uint8_t icon = 0x55,
                       uint8_t srcIcon = 0xFF, uint8_t fmt = 0x60);
        bool postHidePopup();
        bool postFullscreen(const char* l1, const char* l2, const char* l3);
        bool postConfirmBox(const char* caption, const char* row0, const char* row1);
        // Mode 0x05 message box with a real button count (0-2). `labels` is
        // comma-separated ("YES,NO"), each truncated to its 6-byte field.
        // >2 refused: never captured; declared length is 105 + 6*buttons.
        bool postMessageBox(const char* row0, const char* row1, const char* labels,
                            uint8_t buttonCount, uint8_t selected);
        // `03 29 05 <n>` — move the selection between a message box's buttons.
        // NOT postHighlight(), which is the two-row list's `29 01 <rowtag>`.
        bool postSelectButton(uint8_t index);
        // setText with the header exposed; iconBank2 is payload byte 4 (suspected
        // source of the always-lit CD icon, unconfirmed).
        bool postTextStyled(const char* text, uint8_t icon, uint8_t srcIcon,
                            uint8_t fmt, uint8_t iconBank2);
        bool postInfoPopup(const char* l1, const char* l2, const char* l3);
        // wire=false: as if the panel sent it (KeySource::Local). wire=true:
        // transmit the panel's key frame on the bus (KeySource::Wire), for a
        // factory radio sharing it; carried in p[0].
        bool postKey(uint16_t wireCode, bool hold, bool wire = false);
        bool postAbort();
        bool postResync();
        bool postClear();

        // Requests waiting, the rows slot included. The panel task sleeps a
        // single tick, not 2 ms, while this is non-zero.
        uint32_t pending() const;

        // Call from the task that owns poll(), and only from there.
        // Drains everything queued, then the rows slot; returns how many
        // requests were applied.
        uint8_t drain(affa::AffaDisplayBase& d);

        // The panel task's onComplete, for every ticket the library finishes:
        // a tracked request's verdict goes on as a Completion.
        void libComplete(affa::TxTicket t, affa::Result r);

        uint32_t posted() const { return _posted.load(std::memory_order_relaxed); }
        uint32_t dropped() const { return _dropped.load(std::memory_order_relaxed); }
        // Rows requests a newer one replaced before they were drained.
        uint32_t rowsCoalesced() const { return _rowsCoalesced.load(std::memory_order_relaxed); }

        // ---- the panel task's counts, published in PanelStatus ---------------
        // Deliberate screens accepted (the ui holds the glass on each new one).
        uint16_t deliberateSeq() const { return _deliberateSeq; }
        uint32_t clears() const        { return _clears; }
        uint32_t resyncs() const       { return _resyncs; }
        // Automatic repaints dropped because a deliberate screen overtook them.
        uint32_t yielded() const       { return _yielded; }
        const MailboxIds::TicketMap& tickets() const { return _map; }

        // The ops a person's screen is drawn with: accepted and `deliberate`,
        // they hold the glass against the automatic repaints. Not the popup (an
        // overlay; the base keeps redrawing), nor time, power, key or abort.
        static bool holdsGlass(Op o);

        static const char* opName(Op o);

    private:
        // One request: the library call, its id bookkeeping, the report.
        void apply(affa::AffaDisplayBase& d, const Req& r);
        affa::Result render(affa::AffaDisplayBase& d, const Req& r);
        void complete(const MailboxIds::Completion& c);

        QueueHandle_t _q = nullptr;
        affa::CarminatDisplay* _carminat = nullptr;
        PaneBuffers*  _pane = nullptr;
        ReportFn      _report = nullptr;
        void*         _ctx = nullptr;
        CompletionFn  _done = nullptr;
        void*         _doneCtx = nullptr;
        MailboxIds::IdGen     _ids;
        MailboxIds::TicketMap _map;          // the panel task only

        // The coalescing rows slot. A copy under a spinlock: ~120 bytes.
        mutable Locks::Spin _rowsLock;
        Req                 _rows;
        volatile bool       _rowsFull = false;

        // Bumped by every poster: the loop, httpd, and since Step 2 the keys
        // task (popups, macros) and the hu task (head-unit text). Atomic, or
        // two posts at once lose a count.
        std::atomic<uint32_t> _posted{0};
        std::atomic<uint32_t> _dropped{0};
        std::atomic<uint32_t> _rowsCoalesced{0};
        // The panel task's alone.
        uint16_t _deliberateSeq = 0;
        uint32_t _clears = 0, _resyncs = 0, _yielded = 0;
    };

    // The Carminat fullscreen (0x21 05), built here so its geometry can be
    // chosen. Measured on the bench panel:
    //   * text is read from content byte 32; starting earlier loses characters;
    //   * a row shows ~22 wide characters;
    //   * header byte 5 is the body length, declLen - 0x20 (OEM: 0x69 / 0x49);
    //   * the library's 0x60 leaves 64 bytes for three rows and starves row 3;
    //     0x69 leaves 73.
    // Rows are 0x0D-terminated; an empty row is skipped. b5 = 0 derives it from
    // declLen; other values are for sweeping.
    static constexpr uint8_t FS_COLS = 22;
    affa::Result sendFullscreen(affa::CarminatDisplay& d, const char* l1, const char* l2,
                                const char* l3, uint8_t declLen = 0x69,
                                uint8_t b5 = 0, uint8_t start = 32);
}
