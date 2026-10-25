#pragma once
#include <AffaDisplay.h>
#include <Arduino.h>
#include <atomic>
#include "GlassGate.h"
#include "KeyVerdicts.h"
#include "../utils/Locks.h"

// Every render this firmware makes, from whichever task makes it, and the
// glass policy the library cannot know about.
//
// AffaDisplay takes a render from any task once its poll owner is declared
// (App::Panel::begin): off the owner the call copies its finished bytes into
// the library's dispatch ring and hands back the ticket at once; the owner
// admits it inside its next poll(), after the keys. So nothing here queues:
// submit() is the library call, on the caller's task. pressKey, the aborts
// and begin() (Resync) are posted by the library the same way.
//
// What stays here is policy:
//   * a person's screen (the web console, a macro) moves deliberateSeq() when
//     accepted, and the ui task holds the glass on each new value;
//   * an automatic repaint (`yields`) carries the deliberateSeq() its poster had
//     seen; if a person's screen was accepted since, it is refused Aborted here
//     instead of covering that screen. The check and the render are one step
//     under a lock (GlassGate.h), so the two cannot interleave;
//   * Clear is only a count, which the ui task acts on; Resync is counted too.
namespace MegaOpen
{
    class PanelOps {
    public:
        // Every rendering call the library offers, so the web console can
        // exercise the whole surface. The popup is the only overlay and the
        // only screen with a hide; every other screen is replaced by the next
        // render (see main.cpp).
        enum class Op : uint8_t {
            Text, TextStyled, Time, Power,
            Menu, Highlight,
            Popup, HidePopup,
            Fullscreen,
            ConfirmBox, MessageBox, SelectButton,
            InfoPopup,
            Key, Abort,
            // Not renders. Resync re-runs the handshake (the library's begin());
            // Clear hands the glass back to the media screen: it is counted
            // (clears()) and the ui task acts on the count.
            Resync, Clear,
            // The 48x48 pane from r.img; the library copies it at the call.
            NavBitmap,
        };

        static constexpr uint8_t STR_MAX = 33;   // 32 chars + NUL

        struct Req {
            Op       op = Op::Text;
            char     a[STR_MAX] = {0};
            char     b[STR_MAX] = {0};
            char     c[STR_MAX] = {0};
            uint16_t u16 = 0;      // key wire code, menu scroll byte, or row index
            bool     flag = false; // power on / key hold

            // Did a person ask for this screen? Accepted, it holds the glass
            // against automatic repaints. Not implied by "came through here":
            // the ui task's own screens pass false, or the hold would starve
            // the pane. Defaults true (safe side).
            bool     deliberate = true;
            // Per-op extra bytes: popup icon, source icon, format — exposed so
            // the console can sweep them.
            uint8_t  p[4] = {0, 0, 0, 0};

            // An automatic repaint: refused Aborted if a deliberate screen was
            // accepted after `epoch` (the deliberateSeq() the poster had seen).
            bool     yields = false;
            uint16_t epoch = 0;
            // The caller reports the verdict itself (the ui task): a refusal
            // is not logged here.
            bool     quiet = false;
            // NavBitmap: MediaPane::BYTES of image.
            const uint8_t* img = nullptr;
        };

        // setup(), before any task renders. `carminat` is the same object
        // typed (styled text, message boxes, the pane); null on UpdateList.
        void attach(affa::AffaDisplayBase* d, affa::CarminatDisplay* carminat);
        bool attached() const { return _d != nullptr; }

        // Any task. The library's verdict at the call: a ticket whose answer
        // comes through onComplete, or the reason it was refused. Ops that are
        // not renders return no ticket, only their result.
        affa::Submitted submit(const Req& r);

        // `deliberate` false = firmware-initiated render (see Req::deliberate).
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

        // The panel task's onComplete: true when `t` is a key posted through
        // here, whose verdict is then logged if it is a refusal. Not a render's.
        bool keyDone(affa::TxTicket t, affa::Result r);

        // Accepted and refused calls, every task's.
        uint32_t posted() const  { return _posted.load(std::memory_order_relaxed); }
        uint32_t refused() const { return _refused.load(std::memory_order_relaxed); }

        // ---- for the ui task and the panel task -------------------------------
        // Deliberate screens accepted (the ui holds the glass on each new one).
        uint16_t deliberateSeq() const { return _glass.seq(); }
        uint32_t clears() const        { return _clears.load(std::memory_order_relaxed); }
        uint32_t resyncs() const       { return _resyncs.load(std::memory_order_relaxed); }
        // Automatic repaints refused because a deliberate screen overtook them.
        uint32_t yielded() const       { return _glass.yielded(); }

        // The ops a person's screen is drawn with: accepted and `deliberate`,
        // they hold the glass against the automatic repaints. Not the popup (an
        // overlay; the base keeps redrawing), nor time, power, key or abort.
        static bool holdsGlass(Op o);

        static const char* opName(Op o);

    private:
        affa::Submitted render(const Req& r);
        void logKey(affa::Result r);

        affa::AffaDisplayBase* _d = nullptr;
        affa::CarminatDisplay* _carminat = nullptr;

        // Every task posts: the loop, httpd, the keys task (popups, macros),
        // the hu task (head-unit text) and the ui task.
        std::atomic<uint32_t> _posted{0}, _refused{0};
        std::atomic<uint32_t> _clears{0}, _resyncs{0};
        // Keys posted from another task, until their verdict arrives.
        KeyVerdicts           _keys;

        // The glass check's lock: a FreeRTOS mutex (priority inheritance, so a
        // keys-task macro waiting on the ui task is not inverted), held for
        // one ring push.
        static constexpr uint32_t GLASS_WAIT_MS = 20;
        struct GlassLock {
            Locks::Mutex m;
            bool lock()   { return m.take(GLASS_WAIT_MS); }
            void unlock() { m.give(); }
        };
        GlassGate<GlassLock> _glass;
    };

    // Wide characters per fullscreen row on a Carminat (camera-measured; the
    // library's showFullscreenText lays rows out for it since v1.1.0).
    static constexpr uint8_t FS_COLS = affa::carminat::kFullscreenCols;
}
