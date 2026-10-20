#include "UiTask.h"
#if FEATURE_DISPLAY_CAN
#include "PanelOwner.h"
#include "OneShot.h"
#include "Rt.h"
#include "../cfg/Config.h"
#include "../diag/CanBench.h"
#include "../display/AffaMailbox.h"
#include "../display/AffaPort.h"
#include "../display/PaneBuffers.h"
#include "../display/TextFold.h"
#include "../keys/KeyEngine.h"
#include "../media/MediaRouter.h"
#include "../media/HuMediaSource.h"
#include "../utils/LogBus.h"
#include "freertos/queue.h"
#include <time.h>

// The ui's wait must outlive the library's longest legitimate ticket: a held
// job, then every ACKed attempt with its backoff and
// dirty-quiet gap. MailboxIds.h cannot see AffaConfig, so it is checked here.
static_assert(MailboxIds::Waits::LIB_WORST_MS >=
                  (uint32_t)AFFA_TX_HOLD_MS +
                  (uint32_t)AFFA_ACK_TIMEOUT_MS * (AFFA_TX_MAX_RETRIES + 1) +
                  (uint32_t)AFFA_TX_RETRY_MS * ((1u << AFFA_TX_MAX_RETRIES) - 1) +
                  (uint32_t)AFFA_TX_DIRTY_QUIET_MS * AFFA_TX_MAX_RETRIES,
              "Waits::TIMEOUT_MS is shorter than the library's ticket lifetime");
static_assert(MailboxIds::Waits::TIMEOUT_MS > MailboxIds::Waits::LIB_WORST_MS,
              "Waits::TIMEOUT_MS needs a margin over the library's worst case");

// The render policy, moved off the panel task (it was loop() until Step 3, then
// UiPolicy inline on the panel task). The rules are the same; what changed is
// that every library call became a post, so each answer that used to be a
// return value now arrives later, as a Completion or as a count in PanelStatus.
// The state keeps its old names, so the history of each rule still reads
// across the moves.
namespace App
{
    namespace Ui
    {
        namespace
        {
            using MegaOpen::AffaMailbox;
            using MailboxIds::ReqId;
            using MailboxIds::NO_REQ;

            constexpr uint32_t WAIT_MS   = 50;      // the pass rate with nothing to answer
            constexpr uint8_t  UIQ_DEPTH = 16;
            constexpr uint32_t LOG_MS    = 10000;   // the [ui] line

            Deps s_d;
            int  s_id = -1;

            // ---- the UiQ: verdicts from the panel task --------------------------
            StaticQueue_t          s_qBuf;
            uint8_t                s_qStore[UIQ_DEPTH * sizeof(MailboxIds::Completion)];
            QueueHandle_t          s_q = nullptr;
            std::atomic<uint32_t>  s_qDrops{0};

            // What each tracked request was for, and the ones still owed.
            enum Tag : uint8_t { T_CLOCK = 1, T_PANE, T_ROWS, T_SPLASH, T_NOTIFY, T_PROBE, T_HIDE };
            MailboxIds::Waits s_waits;
            uint32_t          s_completions = 0, s_stray = 0;

            // ---- the now-playing screen. Off until asked ----------------------
            MegaOpen::MediaScreen g_media3;
            bool                  g_mediaAuto = false;

            // Scroll indicator beside the rows. Default none: now-playing is not a list.
            uint8_t g_mediaScroll = 0x00;   // carminat::kScrollNone

            // "Newer data exists"; set by either source, cleared on this task
            // before it reads under the source's own lock, so a racing set is
            // seen on the next pass.
            volatile bool g_mediaPending = false;
            uint32_t      s_mediaLockMisses = 0;

            // The rows request still owed an answer, and the paint floor's clock.
            ReqId    g_rowsReq = NO_REQ;
            uint32_t s_lastPaintMs = 0;

            // ---- the 48x48 pane, the graphic half of the same screen ----------
            // A layer on 0x1F1 owning half the glass. Only setText and
            // showInfoMenu leave it visible (Layout::Info, Layout::Text); menu
            // and fullscreen cover it. See MediaPane.h.
            MegaOpen::MediaPane g_pane;

            // The two buffers are PaneBuffers' (the library borrows them; see
            // there). The next image is composed into s_paneLast ^ 1, and
            // s_paneLast holds the last one posted: the sameness reference.
            uint8_t s_paneLast = 1;
            // In flight, or NO_REQ. One pane at a time: rate-limits to the wire.
            ReqId    g_paneReq = NO_REQ;
            uint32_t g_panesSent = 0, g_panesFailed = 0;
            // Send the next composition even if byte-identical to the last posted one.
            bool g_paneResend = false;
            // Animation steps per delivered image — see Cfg::Settings::paneSpeed.
            uint8_t g_paneSpeed = 1;
            // Carousel. g_paneAuto comes from the settings; the deadline is this
            // task's alone. 0 = not armed.
            bool     g_paneAuto     = false;
            uint16_t g_paneCycleS   = 30;
            uint32_t g_paneCycleAtMs = 0;

            // ---- the track splash ---------------------------------------------
            // On a title change in the pane layouts (8 columns), a fullscreen
            // title/artist/album takes the glass for g_splashMs. AMS sends
            // attributes tens of ms apart, so the splash is owed for
            // SPLASH_SETTLE_MS before it is composed.
            uint16_t g_splashMs      = 4000;     // 0 = off; from NVS, editable
            uint32_t g_splashDueMs   = 0;        // non-zero: owed at this time
            uint32_t g_splashUntilMs = 0;        // non-zero: on the glass until then
            ReqId    s_splashReq     = NO_REQ;
            constexpr uint32_t SPLASH_SETTLE_MS = 300;   // AMS sends title, artist, album ~10-70 ms apart
            inline bool splashUp(uint32_t now)
            {
                return g_splashUntilMs && (int32_t)(now - g_splashUntilMs) < 0;
            }

            // A phone notification, drawn as a fullscreen (the popup's frame gets
            // overwritten by the Text marquee). Rides g_splashUntilMs. Written on
            // the NimBLE task, drawn on this one: rows cross under g_notifyMux;
            // g_notifySeq detects newer rows.
            constexpr uint32_t NOTIFY_MS = 6000;
            portMUX_TYPE g_notifyMux = portMUX_INITIALIZER_UNLOCKED;
            bool     g_notifyDue = false;
            uint32_t g_notifySeq = 0;
            char     g_notifyRows[3][MegaOpen::MediaScreen::ROW_MAX] = {};
            ReqId    s_notifyReq = NO_REQ;

            // ---- who owns the glass right now ----------------------------------
            // Held when the panel task accepts a deliberate screen (its count in
            // PanelStatus moves); the automatic repaints (media rows, pane,
            // splash) stand down until it expires. A deadline, not a flag, so
            // the media screen comes back on its own. g_glassHoldMs 0 = hold
            // indefinitely. Repaints carry s_holdSeq, the count acted on: the
            // panel drops one decided before a newer deliberate screen.
            uint32_t g_glassHeldUntilMs = 0;
            uint32_t g_glassHoldMs = 10000;
            uint16_t s_holdSeq = 0;
            // True while a posted screen owns the glass. False on a fresh boot.
            inline bool glassHeld(uint32_t now)
            {
                if (!g_glassHeldUntilMs) return false;
                if (!g_glassHoldMs) return true;            // held until explicitly released
                return (int32_t)(now - g_glassHeldUntilMs) < 0;
            }

            // Push the clock to the panel (NVS, runtime-editable). Sources: the
            // phone, and the head unit's 0x9D (see postHuClock).
            bool g_autoTime = true;

            // ---- the clock, as a tracked one-shot -------------------------------
            // Latest HH:MM from the phone; pending until confirmed drawn.
            char     g_clockHhmm[5] = {0};
            bool     g_clockPending = false;
            ReqId    g_clockReq     = NO_REQ;
            // Who set it last, and to what (GET /api/media clockSrc, clockAt).
            ClockSrc g_clockSrc = ClockSrc::None;
            uint16_t g_clockAt  = 0;
            void clockFrom(ClockSrc src, uint16_t hm) { g_clockSrc = src; g_clockAt = hm; }
            // Earliest retry time; with the single request, stops a failing
            // render owning the bus.
            uint32_t g_clockNextMs  = 0;

            // ---- the popup, as a deadline ---------------------------------------
            // millis() to take the popup down, or 0. This task's alone: the keys
            // task, macros and the web ask for it through g_armPopupReq.
            uint32_t g_popupUntilMs = 0;
            ReqId    s_hideReq = NO_REQ;

            // ---- requests from other tasks --------------------------------------
            // One-shot slots, latest wins (app/OneShot.h). Taken at the top of
            // every pass; nothing else writes the state behind them.
            OneShot<uint32_t> g_armPopupReq;      // take the popup down this many ms from now
            OneShot<uint8_t>  g_releaseGlassReq;  // hand the glass back to the repaints
            OneShot<uint8_t>  g_sceneReq;         // a macro's pane scene (MediaPane::Scene)
            OneShot<uint16_t> g_clockReqSlot;     // the head unit's 0x9D: hours << 8 | minutes
            OneShot<uint16_t> g_manualClockSlot;  // /api/display/clock, already posted
            OneShot<uint8_t>  g_splashNowReq;     // the media page's "splash now"

            // ---- the key popup ---------------------------------------------------
            // Shows what was pressed and what the head unit was told.
            // Runtime-switchable. Shares g_popupUntilMs: the panel has one popup
            // layer, so one deadline.
            bool     g_keyPopup   = true;
            uint16_t g_keyPopupMs = 2000;
            std::atomic<uint32_t> g_keyPopups{0};

            // ---- resyncs ---------------------------------------------------------
            // PEER_ALIVE is not a health flag: set by the panel's ~1 Hz `69` ping
            // and cleared on the next heartbeat (sync 0x0A and 0x08 are both
            // healthy). What counts is losing FUNCSREG, when the peer deadline
            // expires and the queue is cancelled.
            uint32_t g_resyncs = 0;         // times registration was lost since boot
            uint32_t g_lastResyncMs = 0;
            bool     g_wasRegistered = false;
            // The last resync, asked or automatic: 30 s between automatic ones.
            uint32_t s_lastAutoMs = 0;
            // Resyncs this task asked for and has not yet seen drained: the
            // drain is not then logged a second time as "requested".
            uint32_t s_ownResyncs = 0;

            // ---- the panel's counts, as last acted on ----------------------------
            uint32_t s_syncSeen = 0, s_clearsSeen = 0, s_resyncsSeen = 0;

            // ---- the render circuit breaker --------------------------------------
            // Consecutive delivery failures (PanelStatus::failStreak). A
            // three-row screen is 14 frames; repainting into failures drove the
            // controller to BUS_OFF on the bench. Past the threshold the repaint
            // backs off to one attempt every few seconds until something completes.
            constexpr uint16_t kFailStreakTrip = 5;

            // ---- the bring-up self-test: text only once the panel has answered ---
            uint32_t s_nextTryMs = 8000;
            uint8_t  s_tries = 0;
            bool     s_ok = false;
            bool     s_wasListenOnly = true;
            bool     s_wasSynced = false;
            ReqId    s_probeReq = NO_REQ;

            // Something was posted this pass: the panel's busy() cannot know yet.
            bool s_postedThisPass = false;

            // ---- what the web reads ------------------------------------------------
            Staged<MediaSettings>          s_settings;
            Staged<PaneBits>               s_paneBits;
            uint16_t                       s_sceneSeqSeen = 0, s_pokeSeqSeen = 0;
            Snapshot<UiStatus>             s_status;
            Mirror<MegaOpen::MediaScreen>  s_mediaMirror;
            Mirror<MegaOpen::MediaPane>    s_paneMirror;

            // The [ui] line's window.
            uint32_t s_busyWinUs = 0;

            // `s` over two rows of `cols`, broken at spaces; what does not fit
            // ends the second row with "..". Rows must hold cols + 1.
            void wrapTwoEllipsis(const char* s, uint8_t cols, char* a, char* b)
            {
                auto take = [](const char*& p, uint8_t cols, char* out) {
                    while (*p == ' ') ++p;
                    size_t len = strlen(p);
                    size_t n = len <= cols ? len : cols;
                    if (len > cols) {                       // break at the last space that fits
                        size_t k = cols;
                        while (k > 0 && p[k] != ' ') --k;
                        if (k > 0) n = k;
                    }
                    memcpy(out, p, n);
                    out[n] = '\0';
                    p += n;
                };
                const char* p = s;
                take(p, cols, a);
                take(p, cols, b);
                while (*p == ' ') ++p;
                if (*p) {                                   // more left: mark the cut
                    size_t n = strlen(b);
                    if (n + 2 > cols) n = cols - 2;
                    while (n > 0 && b[n - 1] == ' ') --n;
                    b[n] = '.'; b[n + 1] = '.'; b[n + 2] = '\0';
                }
            }

            // ---- posting ---------------------------------------------------------
            // Every request this task makes: tracked, so its verdict comes back.
            ReqId postTracked(AffaMailbox::Req& r, Tag tag, uint32_t now)
            {
                r.track = true;
                const ReqId id = s_d.box->post(r);
                if (id == NO_REQ) return NO_REQ;
                s_postedThisPass = true;
                // A full table cannot time the request out, so it is not waited on.
                if (!s_waits.add(id, tag, now)) return NO_REQ;
                return id;
            }

            // Three rows as a fullscreen: the OEM geometry on a Carminat (rows
            // read from byte 32, ~22 columns), the library's otherwise.
            void fullscreenReq(AffaMailbox::Req& r, bool carminat,
                               const char* l1, const char* l2, const char* l3)
            {
                auto put = [](char* dst, const char* s) {
                    snprintf(dst, AffaMailbox::STR_MAX, "%s", s ? s : "");
                };
                r.op = carminat ? AffaMailbox::Op::FullscreenGeom : AffaMailbox::Op::Fullscreen;
                put(r.a, l1); put(r.b, l2); put(r.c, l3);
                r.p[0] = 0x69; r.p[1] = 0; r.p[2] = 32;
                r.deliberate = false;
            }

            // MediaInfo -> the three rows. Not BLE-gated: the head unit is also
            // a source.
            void fillMediaScreen(const MediaInfo& m, uint32_t nowMs, bool linkUp)
            {
                // Fold to ASCII before MediaScreen, which measures the marquee in
                // bytes (UTF-8 Cyrillic would scroll at half speed and split
                // mid-sequence). normalizeTitle also strips the Apple Music video
                // tag ("• Є відео"). Static: this task alone calls it, and its
                // stack is sized without them (TaskPlan.h, UI_STACK).
                static char t[MegaOpen::MediaScreen::ROW_MAX * 2];
                static char a[MegaOpen::MediaScreen::ROW_MAX * 2];

                // The head unit reports no play state or position (Raise protocol).
                g_media3.setHasTransport(s_d.router->activeSource() != s_d.hu);

                if (!linkUp) {
                    // Show the missing source rather than leaving a stale track up.
                    affa::toAscii(s_d.router->statusText(), t, sizeof(t));
                    const char* none = (s_d.router->activeSource() == s_d.hu) ? "NO HEAD UNIT" : "NO PHONE";
                    g_media3.setTitle(t[0] ? t : none);
                    g_media3.setArtist("");
                    g_media3.setAlbum("");
                    g_media3.setState(MegaOpen::MediaScreen::State::Paused);
                    g_media3.setProgress(0, 0);
                    return;
                }

                // AMS puts the video tag on the artist too.
                static char pre[MegaOpen::MediaScreen::ROW_MAX * 4];
                MegaOpen::preFold(m.title.c_str(), pre, sizeof(pre));
                affa::normalizeTitle(pre, t, sizeof(t));
                MegaOpen::preFold(m.artist.c_str(), pre, sizeof(pre));
                affa::normalizeTitle(pre, a, sizeof(a));

                // Empty title is normal for a freshly paired phone (nothing played yet).
                g_media3.setTitle(t[0] ? t : "---");
                g_media3.setArtist(a);
                // Only the `flow` style draws it. Reuses `a`: setArtist() already copied.
                MegaOpen::preFold(m.album.c_str(), pre, sizeof(pre));
                affa::normalizeTitle(pre, a, sizeof(a));
                g_media3.setAlbum(a);

                switch (m.playbackState) {
                    case MediaInfo::PlaybackState::Playing:
                        g_media3.setState(MegaOpen::MediaScreen::State::Playing); break;
                    case MediaInfo::PlaybackState::Rewinding:
                        g_media3.setState(MegaOpen::MediaScreen::State::Rewinding); break;
                    case MediaInfo::PlaybackState::FastForwarding:
                        g_media3.setState(MegaOpen::MediaScreen::State::Forwarding); break;
                    default:
                        g_media3.setState(MegaOpen::MediaScreen::State::Paused); break;
                }
                g_media3.setProgress((uint16_t)m.elapsedAt(nowMs), (uint16_t)m.duration);
            }

            // The only place MediaScreen is written from the sources. Refills when:
            //   1. the track changed (g_mediaPending);
            //   2. once a second, the position (AMS only reports changes;
            //      elapsedAt() extrapolates; a bar cell is ~18 s of a 3-minute track);
            //   3. the router's active source or its liveness changed.
            void serviceMediaScreen(uint32_t now)
            {
                // The head-unit source expires on silence, so it needs the time.
                s_d.hu->setNow(now);

                static MediaInfo     s_shown;
                static uint32_t      s_mediaMs = 0;
                static IMediaSource* s_wasSrc = nullptr;
                static int8_t        s_wasLive = -1;      // -1: not yet decided either way

                IMediaSource* const src = s_d.router->activeSource();
                const bool live = s_d.router->active();

                if (g_mediaPending || src != s_wasSrc || s_wasLive != (int8_t)live) {
                    // The copy is taken under the source's mutex, which the AMS
                    // writer (NimBLE) or a web reader may hold: at most 5 ms, and
                    // only this task waits for it now, at priority 2. A miss
                    // consumes nothing, so the refill happens on a later pass.
                    MediaInfo fresh;
                    if (live && !s_d.router->trySnapshot(fresh, 5)) {
                        ++s_mediaLockMisses;
                        return;
                    }
                    g_mediaPending = false;
                    s_wasSrc  = src;
                    s_wasLive = (int8_t)live;
                    s_shown = live ? fresh : MediaInfo{};
                    fillMediaScreen(s_shown, now, live);
                    s_mediaMs = now;

                    // A new (normalised) title owes a splash; only while live.
                    static char s_lastTitle[MegaOpen::MediaScreen::ROW_MAX * 2] = {0};
                    if (live && strcmp(s_lastTitle, g_media3.title()) != 0) {
                        snprintf(s_lastTitle, sizeof(s_lastTitle), "%s", g_media3.title());
                        g_media3.restartMarquee(now);
                        if (g_splashMs && strcmp(s_lastTitle, "---") != 0)
                            g_splashDueMs = now + SPLASH_SETTLE_MS;
                    }

                    // Single-line fallback when the three-row screen is off. Posted
                    // with deliberate=false, or it would arm the glass hold and
                    // starve the pane.
                    if (!g_mediaAuto && live && s_d.box->postText(g_media3.title(), /*deliberate=*/false))
                        s_postedThisPass = true;
                } else if (live && (uint32_t)(now - s_mediaMs) >= 1000) {
                    s_mediaMs = now;
                    // Only where there is a position (not the head unit).
                    if (g_media3.hasTransport())
                        g_media3.setProgress((uint16_t)s_shown.elapsedAt(now),
                                             (uint16_t)s_shown.duration);
                }
            }

            // ---- the web's settings and pokes, applied here --------------------------
            void applySettings()
            {
                // The picture before the settings: a POST that sets both stages
                // the picture first, so the Custom scene never shows the old one
                // for longer than a pass. The pane borrows the live buffer; the
                // swap made the old one the staging buffer, so re-point it now.
                if (s_paneBits.applyIfDirty()) g_pane.setCustom(s_paneBits.live().bits);
                if (!s_settings.applyIfDirty()) return;
                const MediaSettings& m = s_settings.live();
                g_mediaAuto   = m.mediaAuto;
                g_mediaScroll = m.scroll;
                g_media3.setLayout(m.layout);
                g_media3.setStyle(m.style);
                g_media3.setCols(m.cols);
                g_media3.setBarWidth(m.bar);
                g_media3.setScrollMsTitle(m.scrollMsTitle);
                g_media3.setScrollMsArtist(m.scrollMsArtist);
                g_paneAuto   = m.paneAuto;
                g_paneCycleS = m.paneCycleS ? m.paneCycleS : 30;
                g_paneSpeed  = m.paneSpeed ? m.paneSpeed : 1;
                g_splashMs   = m.splashMs;
                g_autoTime   = m.autoTime;

                // A pane choice, once: "auto" starts the carousel without moving
                // the live scene (from Media if the pane was off); a scene stops it.
                if (m.sceneSeq != s_sceneSeqSeen) {
                    s_sceneSeqSeen = m.sceneSeq;
                    if (m.paneAuto) {
                        if (!g_pane.on()) g_pane.setScene(MegaOpen::MediaPane::Scene::Media);
                    } else {
                        g_pane.setScene(m.paneScene);
                    }
                    g_pane.ensureSeeded();
                }

                // The media page's test fields: they hold until the source's next update.
                if (m.pokeSeq != s_pokeSeqSeen) {
                    s_pokeSeqSeen = m.pokeSeq;
                    const MediaPoke& p = m.poke;
                    if (p.has & MediaPoke::TITLE)  g_media3.setTitle(p.title);
                    if (p.has & MediaPoke::ARTIST) g_media3.setArtist(p.artist);
                    // The phone overwrites it on the next minute.
                    if (p.has & MediaPoke::CLOCK)  g_media3.setClock(p.clock);
                    if (p.has & MediaPoke::STATE)
                        g_media3.setState((MegaOpen::MediaScreen::State)p.state);
                    if (p.has & (MediaPoke::ELAPSED | MediaPoke::DURATION))
                        g_media3.setProgress((p.has & MediaPoke::ELAPSED) ? p.elapsed : g_media3.elapsed(),
                                             (p.has & MediaPoke::DURATION) ? p.duration : g_media3.duration());
                }
            }

            // ---- requests from other tasks for the render state this task owns ------
            void takeRequests(uint32_t now)
            {
                uint32_t popupMs;
                if (g_armPopupReq.take(popupMs)) {
                    g_popupUntilMs = now + popupMs;
                    if (!g_popupUntilMs) g_popupUntilMs = 1;            // 0 means "none"
                }
                uint8_t one;
                if (g_releaseGlassReq.take(one)) g_glassHeldUntilMs = 0;
                uint8_t scene;
                if (g_sceneReq.take(scene)) {
                    g_pane.setScene((MegaOpen::MediaPane::Scene)scene);
                    g_pane.ensureSeeded();
                }
                if (g_splashNowReq.take(one)) g_splashDueMs = now | 1;
                uint16_t hm;
                if (g_clockReqSlot.take(hm)) {
                    char hhmm[5];
                    snprintf(hhmm, sizeof(hhmm), "%02u%02u", (unsigned)(hm >> 8), (unsigned)(hm & 0xFF));
                    if (strcmp(hhmm, g_clockHhmm) != 0) {           // a new minute
                        memcpy(g_clockHhmm, hhmm, sizeof(hhmm));
                        // The policy draws it and retries until delivered.
                        if (g_autoTime) g_clockPending = true;
                        g_media3.setClock(g_clockHhmm);
                    }
                    if (g_autoTime) clockFrom(ClockSrc::Hu, hm);
                }
                // Drawn by the web already: only the record and the header.
                if (g_manualClockSlot.take(hm)) {
                    snprintf(g_clockHhmm, sizeof(g_clockHhmm), "%02u%02u",
                             (unsigned)(hm >> 8), (unsigned)(hm & 0xFF));
                    g_media3.setClock(g_clockHhmm);
                    clockFrom(ClockSrc::Manual, hm);
                }
            }

            // ---- what the panel task did since the last pass ----------------------
            // Counts, not events, so nothing is lost however long this task was
            // away; each one stands for a callback the old inline policy got.
            void watchPanel(const MegaOpen::PanelStatus& ps, uint32_t now)
            {
                // Sync changed (either edge): re-owe the clock, and resend the
                // pane: the panel forgets every layer on a new session, and the
                // pane only sends on change. The waits are dropped as before;
                // the panel still releases the pane's buffer on its verdict.
                if (ps.syncChanges != s_syncSeen) {
                    s_syncSeen = ps.syncChanges;
                    if (g_clockHhmm[0]) g_clockPending = true;
                    if (g_clockReq) { s_waits.forget(g_clockReq); g_clockReq = NO_REQ; }
                    g_paneResend = true;
                    if (g_paneReq) { s_waits.forget(g_paneReq); g_paneReq = NO_REQ; }
                    // And the rows: they are credited at post time, so a new
                    // session would otherwise keep a blank glass until they change.
                    g_media3.invalidate();
                }

                // A person's screen was accepted: hold the glass from now.
                if (ps.deliberateSeq != s_holdSeq) {
                    s_holdSeq = ps.deliberateSeq;
                    g_glassHeldUntilMs = now + g_glassHoldMs;
                    if (!g_glassHeldUntilMs) g_glassHeldUntilMs = 1;
                }

                // Clear: ends a splash or notification, takes a popup down, drops
                // the glass hold and forces the text and the pane to be sent again.
                if (ps.clears != s_clearsSeen) {
                    s_clearsSeen = ps.clears;
                    g_glassHeldUntilMs = 0;
                    if (g_splashUntilMs) g_splashUntilMs = now;
                    g_popupUntilMs = now ? now : 1;
                    g_media3.invalidate();
                    g_paneResend = true;
                }

                // The handshake was re-run: re-owe the clock, and hold the 30 s
                // automatic resync off for a full period.
                if (ps.resyncsAsked != s_resyncsSeen) {
                    const uint32_t n = ps.resyncsAsked - s_resyncsSeen;
                    s_resyncsSeen = ps.resyncsAsked;
                    s_lastAutoMs = now;
                    if (g_clockHhmm[0]) g_clockPending = true;
                    const uint32_t own = n < s_ownResyncs ? n : s_ownResyncs;
                    s_ownResyncs -= own;
                    if (n > own) LogBus::printf("[disp] resync: FSMs reset, queue cleared (requested)");
                }

                const bool listenOnly = Diag::CanBench::listenOnly();
                // Re-arm the bring-up probe when listen-only is switched off.
                if (s_wasListenOnly && !listenOnly) {
                    s_tries = 0; s_ok = false; s_nextTryMs = now + 500;
                    LogBus::printf("[disp] transmit enabled — retrying registration");
                }
                s_wasListenOnly = listenOnly;

                if (ps.synced != s_wasSynced) {
                    s_wasSynced = ps.synced;
                    LogBus::printf("[disp] sync %s", s_wasSynced ? "ESTABLISHED" : "lost");
                    if (s_wasSynced) { s_tries = 0; s_ok = false; s_nextTryMs = now + 200; }
                }

                // ---- the opening phase, logged on change ---------------------------
                // Says what the opening is waiting for, e.g. stuck at Announced (panel
                // never heard us) vs AwaitPeerChannel (panel never opened its 1C1).
                {
                    static uint8_t s_phase = 0;
                    static bool s_phaseSeen = false;
                    if (!s_phaseSeen || ps.phase != s_phase) {
                        LogBus::printf("[disp] phase %s -> %s",
                                       s_phaseSeen ? affa::phaseName((affa::Phase)s_phase) : "(boot)",
                                       affa::phaseName((affa::Phase)ps.phase));
                        s_phase = ps.phase;
                        s_phaseSeen = true;
                    }
                }

                // Why the last session ended.
                {
                    static uint32_t s_lostSeen = 0;
                    if (ps.sessionsLost != s_lostSeen) {
                        s_lostSeen = ps.sessionsLost;
                        LogBus::printf("[disp] session lost (#%u): %s", (unsigned)ps.sessionsLost,
                                       affa::lossReasonName((affa::LossReason)ps.lastLoss));
                    }
                }

                // ---- registration: watch it, and get it back -------------------------
                // Losing FUNCSREG: the panel forgot us and the queue was cancelled.
                if (ps.registered != g_wasRegistered) {
                    g_wasRegistered = ps.registered;
                    if (!ps.registered) {
                        ++g_resyncs;
                        g_lastResyncMs = now;
                        LogBus::printf("[disp] REGISTRATION LOST (#%u) — re-arming", (unsigned)g_resyncs);
                        // Re-owe the clock (the library restores power itself).
                        if (g_clockHhmm[0]) g_clockPending = true;
                        // The queue was cancelled: the credited rows never landed.
                        g_media3.invalidate();
                    } else {
                        LogBus::printf("[disp] registered again after %u ms",
                                       (unsigned)(now - g_lastResyncMs));
                    }
                }

                // Resync after 30 s unregistered: begin() resets the FSMs, re-arms the
                // peer deadline and cancels the queue. 30 s, because normal recovery
                // (`61 11` + lazy 0x70 probes) takes a few seconds. A mailbox op:
                // only the panel task may call begin().
                {
                    const bool stuck = !ps.registered && g_lastResyncMs &&
                                       (uint32_t)(now - g_lastResyncMs) > 30000 &&
                                       (uint32_t)(now - s_lastAutoMs) > 30000;
                    if (stuck) {
                        s_lastAutoMs = now;
                        if (s_d.box->postResync()) {
                            ++s_ownResyncs;
                            s_postedThisPass = true;
                        }
                        if (g_clockHhmm[0]) g_clockPending = true;
                        LogBus::printf("[disp] resync: FSMs reset, queue cleared (unregistered 30 s)");
                    }
                }
            }

            // ---- a verdict: what each kind of request does with its answer ---------
            // `synth` = it never reached the wire (refused at enqueue, nothing to
            // send, or dropped for a deliberate screen); otherwise the panel's own
            // answer. A timeout arrives here as Result::Timeout, not synthesised.
            void answer(Tag tag, ReqId id, affa::Result res, bool synth, uint32_t now)
            {
                const bool ok = res == affa::Result::Ok;
                switch (tag) {
                    case T_CLOCK:
                        // The clock retries on the verdict, not on the post.
                        if (id == g_clockReq) g_clockReq = NO_REQ;
                        if (!ok) g_clockPending = true;
                        break;

                    case T_PANE:
                        // Cleared on every verdict, or the pane wedges.
                        if (id == g_paneReq) g_paneReq = NO_REQ;
                        if (ok) ++g_panesSent;
                        else {
                            // Resend: the undelivered image is still the sameness
                            // reference. A refusal was never a failed delivery.
                            if (!synth) ++g_panesFailed;
                            g_paneResend = true;
                        }
                        break;

                    case T_ROWS: {
                        const bool current = id == g_rowsReq;
                        if (current) g_rowsReq = NO_REQ;
                        // The rows were credited when posted; any repaint that did
                        // not land (refused, wire Timeout/SendFailed/LinkDown,
                        // Cancelled by a resync, Aborted, or our own Waits expiry)
                        // makes the screen dirty again so it repaints. Only the
                        // current request: a superseded one (its wait is forgotten
                        // when the next is posted) never gets here, and must not
                        // re-dirty over the newer rows.
                        if (current && !ok) {
                            g_media3.invalidate();
                            static uint32_t s_lastComplaintMs = 0;
                            if (res != affa::Result::Aborted &&
                                (uint32_t)(now - s_lastComplaintMs) >= 5000) {
                                s_lastComplaintMs = now;
                                LogBus::printf("[media] repaint %s: %s",
                                               synth ? "refused" : "failed",
                                               MegaOpen::resultName(res));
                            }
                        }
                        break;
                    }

                    case T_SPLASH:
                        if (id == s_splashReq) s_splashReq = NO_REQ;
                        // Owed until accepted: a refused splash is owed again.
                        if (synth && !ok) {
                            g_splashUntilMs = 0;
                            if (!g_splashDueMs) g_splashDueMs = now | 1;
                        }
                        break;

                    case T_NOTIFY:
                        if (id == s_notifyReq) s_notifyReq = NO_REQ;
                        // Owed until accepted; the rows are still there (or newer ones).
                        if (synth && !ok) {
                            g_splashUntilMs = 0;
                            portENTER_CRITICAL(&g_notifyMux);
                            g_notifyDue = true;
                            portEXIT_CRITICAL(&g_notifyMux);
                        }
                        break;

                    case T_PROBE: {
                        if (id == s_probeReq) s_probeReq = NO_REQ;
                        // Acceptance is what the probe asks about.
                        const bool accepted = !synth || ok;
                        LogBus::printf("[disp] try %u setText(\"SUCCESS\") -> %s%s", s_tries,
                                       MegaOpen::resultName(res), synth ? " (refused)" : "");
                        if (accepted && res != affa::Result::Timeout && !s_ok) {
                            s_ok = true;
                            LogBus::printf("[disp] REGISTERED AND TEXT ACCEPTED");
                        }
                        break;
                    }

                    case T_HIDE:
                        if (id == s_hideReq) s_hideReq = NO_REQ;
                        // A refusal retries in 500 ms, unless a newer popup has
                        // armed its own take-down meanwhile.
                        if ((synth && !ok) || res == affa::Result::Timeout) {
                            if (!g_popupUntilMs) g_popupUntilMs = now + 500;
                            if (!g_popupUntilMs) g_popupUntilMs = 1;
                        }
                        break;
                }
            }

            void onCompletion(const MailboxIds::Completion& c, uint32_t now)
            {
                ++s_completions;
                uint8_t tag;
                if (!s_waits.take(c.id, tag)) { ++s_stray; return; }   // given up, or replaced
                answer((Tag)tag, c.id, (affa::Result)c.result, c.synth, now);
            }

            // ---- the policy: what to say, and when ---------------------------------
            void policy(const MegaOpen::PanelStatus& ps, uint32_t now)
            {
                const bool listenOnly = Diag::CanBench::listenOnly();
                // The panel's busy() is up to a pass old, and cannot see what was
                // posted but not drained yet.
                auto busy = [&]() {
                    return ps.busy || s_postedThisPass || s_d.box->pending() != 0;
                };

                // ---- the clock ------------------------------------------------------
                // As the first render after a resync it carries the lazy 0x70 probes,
                // so FUNCSREG latches on a 4-byte payload. One in flight, at most one
                // try a second: retrying at loop rate starved every other render.
                if (g_clockPending && !g_clockReq && g_autoTime && g_clockHhmm[0] &&
                    ps.synced && !listenOnly &&
                    (int32_t)(now - g_clockNextMs) >= 0) {
                    // Advanced unconditionally, so a refused post also backs off.
                    g_clockNextMs = now + 1000;
                    AffaMailbox::Req r;
                    r.op = AffaMailbox::Op::Time;
                    snprintf(r.a, sizeof(r.a), "%s", g_clockHhmm);
                    r.deliberate = false;
                    const ReqId id = postTracked(r, T_CLOCK, now);
                    if (id) { g_clockReq = id; g_clockPending = false; }
                    // Not posted: leave the flag set and try again after the backoff.
                }

                // What to say; the blocks below decide when.
                serviceMediaScreen(now);

                // ---- a notification, ahead of everything automatic --------------------
                // Not gated on glassHeld(). Owed until accepted.
                if (g_notifyDue && !s_notifyReq && ps.synced && !listenOnly && !busy()) {
                    char rows[3][MegaOpen::MediaScreen::ROW_MAX];
                    portENTER_CRITICAL(&g_notifyMux);
                    memcpy(rows, g_notifyRows, sizeof(rows));
                    const uint32_t seq = g_notifySeq;
                    portEXIT_CRITICAL(&g_notifyMux);
                    AffaMailbox::Req r;
                    fullscreenReq(r, ps.carminat, rows[0], rows[1], rows[2]);
                    const ReqId id = postTracked(r, T_NOTIFY, now);
                    if (id) {
                        s_notifyReq = id;
                        portENTER_CRITICAL(&g_notifyMux);
                        if (g_notifySeq == seq) g_notifyDue = false;   // a newer one stays owed
                        portEXIT_CRITICAL(&g_notifyMux);
                        g_splashUntilMs = now + NOTIFY_MS;
                        if (!g_splashUntilMs) g_splashUntilMs = 1;
                    }
                }

                // ---- the track splash: same gates as the screen; owed until accepted --
                if (g_splashDueMs && (int32_t)(now - g_splashDueMs) >= 0) {
                    if (!g_splashMs || !g_mediaAuto ||
                        !MegaOpen::MediaScreen::sharesWithPane(g_media3.layout())) {
                        g_splashDueMs = 0;                      // nothing to interrupt
                    } else if (!s_splashReq && ps.synced && !listenOnly && !busy() &&
                               !glassHeld(now) && !splashUp(now)) {   // not over a notification
                        char s1[MegaOpen::MediaScreen::ROW_MAX];
                        char s2[MegaOpen::MediaScreen::ROW_MAX];
                        char s3[MegaOpen::MediaScreen::ROW_MAX];
                        g_media3.composeSplash(s1, s2, s3, MegaOpen::FS_COLS);
                        AffaMailbox::Req r;
                        fullscreenReq(r, ps.carminat, s1, s2, s3);
                        r.yields = true;
                        r.epoch  = s_holdSeq;
                        const ReqId id = postTracked(r, T_SPLASH, now);
                        if (id) {
                            s_splashReq     = id;
                            g_splashDueMs   = 0;
                            g_splashUntilMs = now + g_splashMs;
                            if (!g_splashUntilMs) g_splashUntilMs = 1;      // 0 means "none"
                        }
                    }
                }
                // Splash over: force a repaint of the rows and the pane.
                if (g_splashUntilMs && !splashUp(now)) {
                    g_splashUntilMs = 0;
                    g_media3.invalidate();
                    g_paneResend = true;
                    // Media screen off: nothing repaints over the fullscreen, so hide it.
                    if (!g_mediaAuto) g_popupUntilMs = now ? now : 1;
                }

                const uint16_t failStreak = ps.failStreak;

                // ---- the now-playing screen ------------------------------------------
                // Gates: busy() (a menu is 13 ACKed frames; enqueueing over it only gets
                // Aborted), dirty(), a paint floor (wider while a popup is up or the
                // breaker has tripped), glassHeld(). synced(), not registered():
                // registration is lazy and rides on a render, so gating on it deadlocks.
                // The marquee position derives from the clock, so skipped ticks cost nothing.
                if (g_mediaAuto && ps.synced && !listenOnly && !busy() &&
                    !glassHeld(now) && !splashUp(now)) {
                    static constexpr uint32_t MIN_PAINT_MS = 200;
                    static constexpr uint32_t POPUP_PAINT_MS = 1000;
                    static constexpr uint32_t TRIPPED_PAINT_MS = 4000;
                    const uint32_t minPaint = (failStreak >= kFailStreakTrip) ? TRIPPED_PAINT_MS
                                            : g_popupUntilMs                  ? POPUP_PAINT_MS
                                                                              : MIN_PAINT_MS;
                    // A setText under a popup is drawn inside the popup's frame, so the
                    // Text layout waits; the others are only slowed.
                    const bool underPopup = g_popupUntilMs &&
                        g_media3.layout() == MegaOpen::MediaScreen::Layout::Text;
                    if (!underPopup &&
                        (uint32_t)(now - s_lastPaintMs) >= minPaint && g_media3.dirty(now)) {
                        // No hide between frames: each screen replaces the last (~190 ms,
                        // 14 frames). Info and Text leave room for the pane; Info is three
                        // messages, one per row. Text is one 8-char row (compose() did that).
                        AffaMailbox::Req r;
                        g_media3.compose(now, r.a, r.b, r.c);
                        switch (g_media3.layout()) {
                            case MegaOpen::MediaScreen::Layout::Fullscreen:
                                r.op = AffaMailbox::Op::Fullscreen; break;
                            case MegaOpen::MediaScreen::Layout::Info:
                                r.op = AffaMailbox::Op::InfoPopup; break;
                            case MegaOpen::MediaScreen::Layout::Text:
                                r.op = AffaMailbox::Op::Text; break;
                            default:
                                r.op = AffaMailbox::Op::Menu;
                                r.u16 = g_mediaScroll;
                                break;
                        }
                        r.deliberate = false;
                        r.track  = true;
                        r.yields = true;
                        r.epoch  = s_holdSeq;
                        // The coalescing slot: a newer repaint replaces one not yet
                        // drained, whose wait is dropped with it.
                        if (g_rowsReq) { s_waits.forget(g_rowsReq); g_rowsReq = NO_REQ; }
                        const ReqId id = s_d.box->postRows(r);
                        s_lastPaintMs = now;
                        if (id) {
                            s_postedThisPass = true;
                            if (s_waits.add(id, T_ROWS, now)) g_rowsReq = id;
                            // Credited now; a refusal makes it dirty again (answer()).
                            g_media3.markRendered(now);
                        }
                    }
                }

                // ---- the graphic layer ----------------------------------------------
                // After the text block, so text wins ties for the idle wire (a pane is 44
                // frames). One image in flight at a time is the rate limit. The breaker
                // suspends the pane outright. Not under a held glass or a splash.
                // Not gated on g_pane.on(): Off composes an all-zero image, which is the
                // only way to clear the layer; the sameness check sends it once.
                // Keys first: no new image while a key is fresh. An image is 44 ACKed
                // frames and holds the wire and the panel for a few hundred ms; a skipped
                // animation step costs nothing, a key behind it is felt. Every source
                // (panel, knobs, web) counts: they all pass through the key engine.
                static uint32_t s_keyEvents = 0, s_lastKeyMs = 0;
                static constexpr uint32_t KEY_QUIET_MS = 400;
                const uint32_t keyEvents = s_d.engine ? s_d.engine->events() : 0;
                if (keyEvents != s_keyEvents) {
                    s_keyEvents = keyEvents;
                    s_lastKeyMs = now;
                }
                const bool keyQuiet = !s_lastKeyMs || (uint32_t)(now - s_lastKeyMs) >= KEY_QUIET_MS;
                const uint8_t cur = s_paneLast ^ 1;

                if (ps.carminat && s_d.pane && ps.synced && !listenOnly && keyQuiet &&
                    failStreak < kFailStreakTrip && !glassHeld(now) && !splashUp(now) &&
                    g_paneReq == NO_REQ && !busy() && s_d.pane->free(cur)) {

                    // Fed from the text screen, not the router, so the layers agree.
                    g_pane.setHasTransport(g_media3.hasTransport());
                    g_pane.setPlaying(g_media3.state() == MegaOpen::MediaScreen::State::Playing);
                    g_pane.setProgress(g_media3.elapsed(), g_media3.duration());

                    // Carousel, advanced inside the gate so a scene is not skipped unseen.
                    if (g_paneAuto) {
                        const uint32_t cycleMs = (uint32_t)(g_paneCycleS ? g_paneCycleS : 30) * 1000u;
                        if (!g_paneCycleAtMs) g_paneCycleAtMs = now + cycleMs;
                        else if ((int32_t)(now - g_paneCycleAtMs) >= 0) {
                            g_pane.setScene(MegaOpen::MediaPane::carouselNext(g_pane.scene()));
                            g_paneCycleAtMs = now + cycleMs;
                        }
                        if (!g_paneCycleAtMs) g_paneCycleAtMs = 1;          // 0 means "unarmed"
                    } else {
                        g_paneCycleAtMs = 0;
                    }

                    // The clock face needs seconds. Timeout 0: the default getLocalTime()
                    // blocks up to 5 s.
                    if (g_pane.scene() == MegaOpen::MediaPane::Scene::Clock) {
                        struct tm tmNow;
                        if (getLocalTime(&tmNow, 0))
                            g_pane.setClockSecs((uint32_t)tmNow.tm_hour * 3600u +
                                                (uint32_t)tmNow.tm_min * 60u +
                                                (uint32_t)tmNow.tm_sec);
                    }

                    uint8_t* const buf = s_d.pane->data(cur);
                    // Several steps per image: the wire delivers ~2 images/s.
                    for (uint8_t i = 0; i < (g_paneSpeed ? g_paneSpeed : 1); ++i) g_pane.step();
                    g_pane.compose(buf);

                    // The pane's dirty(): compare with the last posted image. The
                    // reference is last posted, not last delivered, so g_paneResend
                    // (non-Ok verdict, lost session) forces a send.
                    if (g_paneResend ||
                        memcmp(buf, s_d.pane->data(s_paneLast), MegaOpen::PaneBuffers::BYTES) != 0) {
                        // Lent before it is posted; the panel gives it back with
                        // the verdict. Never posted: it is ours again at once.
                        if (s_d.pane->lend(cur)) {
                            const ReqId id = s_d.box->postNavBitmap(cur, s_holdSeq);
                            if (id) {
                                s_postedThisPass = true;
                                g_paneResend = false;
                                s_paneLast = cur;
                                if (s_waits.add(id, T_PANE, now)) g_paneReq = id;
                            } else {
                                s_d.pane->release(cur);
                            }
                        }
                    }
                }

                // ---- take the popup back down ---------------------------------------
                // The panel never removes a popup itself. Not gated on busy(): hidePopup
                // is one frame on its own RenderSlot.
                if (g_popupUntilMs && (int32_t)(now - g_popupUntilMs) >= 0 &&
                    !s_hideReq && ps.registered && !listenOnly) {
                    // Off the deadline first; a refusal re-arms it 500 ms out (answer()).
                    g_popupUntilMs = 0;
                    AffaMailbox::Req r;
                    r.op = AffaMailbox::Op::HidePopup;
                    r.deliberate = false;
                    const ReqId id = postTracked(r, T_HIDE, now);
                    if (id) s_hideReq = id;
                    else    g_popupUntilMs = now + 500;    // not posted: try again shortly
                }

                // The bring-up probe runs only with the media screen off.
                if (!s_ok && !s_probeReq && !g_mediaAuto && !listenOnly && ps.synced &&
                    now >= s_nextTryMs && s_tries < 12) {
                    s_nextTryMs = now + 5000;
                    ++s_tries;
                    // Acceptance only; the verdict says whether it was taken.
                    AffaMailbox::Req r;
                    r.op = AffaMailbox::Op::Text;
                    snprintf(r.a, sizeof(r.a), "SUCCESS");
                    r.deliberate = false;
                    const ReqId id = postTracked(r, T_PROBE, now);
                    if (id) s_probeReq = id;
                }
            }

            void servicePhoneClock(uint32_t now)
            {
                // Only reads BLE state: BleHub::Service() runs on ble_svc.
                if (!s_d.amsActive || !s_d.amsActive()) return;
                const bool connected = s_d.phoneConnected && s_d.phoneConnected();

                // ---- the clock from the phone (BLE Current Time Service) ------------
                // Re-sent every minute: the panel's own clock drifts (~17 min/hour on
                // the bench). setTime coalesces on its own RenderSlot.
                static uint32_t s_clockMs = 0;
                static int      s_lastMin = -1;
                if (!connected) { s_lastMin = -1; }
                if (g_autoTime && connected && s_d.phoneTimeSet && s_d.phoneTimeSet() &&
                    (s_clockMs == 0 || (uint32_t)(now - s_clockMs) >= 5000)) {
                    s_clockMs = now;
                    struct tm t;
                    // Timeout 0, as for the pane's clock: the default waits up to 5 s.
                    if (getLocalTime(&t, 0) && t.tm_min != s_lastMin) {
                        s_lastMin = t.tm_min;
                        // The policy draws it and retries until delivered.
                        snprintf(g_clockHhmm, sizeof(g_clockHhmm), "%02d%02d",
                                 t.tm_hour, t.tm_min);
                        g_clockPending = true;

                        // Also into the now-playing header (the panel's own clock box
                        // is not drawn inside the media screen). Same string, so they agree.
                        g_media3.setClock(g_clockHhmm);
                        clockFrom(ClockSrc::Cts, (uint16_t)(t.tm_hour << 8 | t.tm_min));
                        LogBus::printf("[clock] from phone %02d:%02d", t.tm_hour, t.tm_min);
                    }
                }
            }

            void publish()
            {
                UiStatus u;
                u.panesSent        = g_panesSent;
                u.panesFailed      = g_panesFailed;
                u.glassHeldUntilMs = g_glassHeldUntilMs;
                u.resyncs          = g_resyncs;
                u.lastResyncMs     = g_lastResyncMs;
                u.keyPopups        = g_keyPopups.load(std::memory_order_relaxed);
                u.paneScene        = (uint8_t)g_pane.scene();
                u.clockSrc         = g_clockSrc;
                u.clockAt          = g_clockAt;
                s_status.publish(u);
                // The web composes its previews from these copies. A reader
                // holding one only delays its refresh to the next pass.
                s_mediaMirror.publish(g_media3);
                s_paneMirror.publish(g_pane);
            }

            // The [ui] line: this task's own window. mediaBusy = passes that found
            // the media source's mutex held for 5 ms (this is the only task that
            // waits for it now); waits = requests still owed a verdict.
            void logWindow()
            {
                LogBus::printf("[ui] busyMax=%uus mediaBusy=%u completions=%u stray=%u timeouts=%u "
                               "uiqDrops=%u waits=%u rowsCoalesced=%u panes=%u/%u",
                               (unsigned)s_busyWinUs, (unsigned)s_mediaLockMisses,
                               (unsigned)s_completions, (unsigned)s_stray,
                               (unsigned)s_waits.timeouts(),
                               (unsigned)s_qDrops.load(std::memory_order_relaxed),
                               (unsigned)s_waits.used(), (unsigned)s_d.box->rowsCoalesced(),
                               (unsigned)g_panesSent, (unsigned)g_panesFailed);
                s_busyWinUs = 0;
                s_mediaLockMisses = 0;
            }

            void run(void*)
            {
                uint32_t logAt = millis();
                for (;;) {
                    // A verdict wakes it at once (the pane's next image is due);
                    // otherwise a pass every 50 ms is plenty for 200 ms repaint
                    // floors, a 1 s clock retry and a popup's take-down.
                    MailboxIds::Completion c;
                    const bool got = xQueueReceive(s_q, &c, pdMS_TO_TICKS(WAIT_MS)) == pdTRUE;
                    const uint32_t t0 = micros();
                    uint32_t now = millis();
                    if (got) {
                        onCompletion(c, now);
                        uint8_t n = 0;
                        while (n++ < UIQ_DEPTH && xQueueReceive(s_q, &c, 0) == pdTRUE)
                            onCompletion(c, now);
                    }
                    // Nothing waits for ever: an answer the UiQ lost, or one the
                    // panel never got, is given up after Waits::TIMEOUT_MS (21 s).
                    ReqId id;
                    uint8_t tag;
                    while (s_waits.expire(now, id, tag))
                        answer((Tag)tag, id, affa::Result::Timeout, false, now);

                    s_postedThisPass = false;
                    applySettings();
                    takeRequests(now);
                    const MegaOpen::PanelStatus ps = Panel::status();
                    watchPanel(ps, now);
                    policy(ps, now);
                    servicePhoneClock(now);
                    publish();

                    now = millis();
                    if ((uint32_t)(now - logAt) >= LOG_MS) {
                        logAt = now;
                        logWindow();
                    }
                    const uint32_t busyUs = micros() - t0;
                    if (busyUs > s_busyWinUs) s_busyWinUs = busyUs;
                    Tasks::beat(s_id, busyUs);
                }
            }
        }

        void begin(Cfg::Config& cfg, const Deps& d)
        {
            s_d = d;
            if (!s_q) s_q = xQueueCreateStatic(UIQ_DEPTH, sizeof(MailboxIds::Completion), s_qStore, &s_qBuf);

            const Cfg::Settings& s = cfg.s();
            g_autoTime  = s.autoTime;
            g_mediaAuto = s.mediaScreen;
            g_media3.setLayout(MegaOpen::MediaScreen::layoutFromName(s.mediaLayout.c_str()));
            // Width follows the layout (8 columns beside the pane).
            g_media3.setCols(MegaOpen::MediaScreen::colsFor(g_media3.layout()));
            g_media3.setStyle(MegaOpen::MediaScreen::styleFromName(s.mediaStyle.c_str()));
            g_paneSpeed = s.paneSpeed ? s.paneSpeed : 1;
            g_splashMs  = s.trackSplashMs;
            // The pane, seeded here rather than on the first render. "auto" is
            // the carousel, not a scene (sceneFromName would read it as Off); it
            // starts on Media.
            g_paneAuto   = s.mediaPane == "auto";
            g_paneCycleS = s.paneCycleS ? s.paneCycleS : 30;
            g_pane.setScene(g_paneAuto ? MegaOpen::MediaPane::Scene::Media
                                       : MegaOpen::MediaPane::sceneFromName(s.mediaPane.c_str()));
            g_pane.ensureSeeded();
            // The Custom scene's picture, from NVS; all dark until one is drawn.
            {
                PaneBits pb;
                pb.set = cfg.loadPaneBits(pb.bits, sizeof(pb.bits));
                s_paneBits.init(pb);
                g_pane.setCustom(s_paneBits.live().bits);   // borrowed, see MediaPane.h
            }
            g_keyPopup   = s.keyPopup;
            g_keyPopupMs = s.keyPopupMs;

            // The web's starting point: exactly what is live.
            MediaSettings m;
            m.mediaAuto      = g_mediaAuto;
            m.scroll         = g_mediaScroll;
            m.layout         = g_media3.layout();
            m.style          = g_media3.style();
            m.cols           = g_media3.cols();
            m.bar            = g_media3.barWidth();
            m.scrollMsTitle  = g_media3.scrollMsTitle();
            m.scrollMsArtist = g_media3.scrollMsArtist();
            m.paneAuto       = g_paneAuto;
            m.paneScene      = g_pane.scene();
            m.paneCycleS     = g_paneCycleS;
            m.paneSpeed      = g_paneSpeed;
            m.splashMs       = g_splashMs;
            m.autoTime       = g_autoTime;
            s_settings.init(m);
            publish();
        }

        void start()
        {
            // Critical: a dead ui is a dead screen, which the OTA trial should
            // see; it wakes at least every 50 ms, so it is never stale for 2 s
            // unless something is wrong.
            TaskSpec spec;
            spec.name     = "ui";
            spec.fn       = run;
            spec.stack    = Plan::UI_STACK;
            spec.prio     = Plan::UI_PRIO;
            spec.core     = Plan::UI_CORE;
            spec.critical = true;
            spec.idOut    = &s_id;
            spawn(spec);
        }

        void postCompletion(const MailboxIds::Completion& c)
        {
            if (!s_q || xQueueSend(s_q, &c, 0) != pdTRUE)
                s_qDrops.fetch_add(1, std::memory_order_relaxed);
        }

        void armPopup(uint32_t ms)        { g_armPopupReq.post(ms); }
        void releaseGlass()               { g_releaseGlassReq.post(1); }
        void requestScene(uint8_t scene)  { g_sceneReq.post(scene); }
        void postHuClock(uint16_t hm)     { g_clockReqSlot.post(hm); }
        void noteManualClock(uint16_t hm) { g_manualClockSlot.post(hm); }

        const char* clockSrcName(ClockSrc s)
        {
            switch (s) {
                case ClockSrc::Hu:     return "hu";
                case ClockSrc::Cts:    return "cts";
                case ClockSrc::Manual: return "manual";
                default:               return "";
            }
        }
        void mediaChanged()               { g_mediaPending = true; }
        void splashNow()                  { g_splashNowReq.post(1); }

        // Sender on row 1, body wrapped over rows 2-3, folded to ASCII; drawn by
        // the ui task as a fullscreen. Any task: the rows cross under
        // g_notifyMux. A newer notification replaces the rows of one still owed.
        void queueNotification(const char* sender, const char* text)
        {
            using MegaOpen::MediaScreen;
            const uint8_t cols = MegaOpen::FS_COLS;
            char pre[MediaScreen::ROW_MAX * 6];
            char from[MediaScreen::ROW_MAX];
            char body[MediaScreen::ROW_MAX * 3];
            MegaOpen::preFold(sender, pre, sizeof(pre));
            affa::toAscii(pre, from, sizeof(from));
            MegaOpen::preFold(text, pre, sizeof(pre));
            affa::toAscii(pre, body, sizeof(body));
            char rows[3][MediaScreen::ROW_MAX];
            snprintf(rows[0], sizeof(rows[0]), "%.*s", (int)cols, from);
            wrapTwoEllipsis(body, cols, rows[1], rows[2]);
            portENTER_CRITICAL(&g_notifyMux);
            memcpy(g_notifyRows, rows, sizeof(rows));
            ++g_notifySeq;
            g_notifyDue = true;
            portEXIT_CRITICAL(&g_notifyMux);
        }

        bool keyPopupWanted(bool trackKey)
        {
            if (!g_keyPopup) return false;
            return !(trackKey && g_splashMs && g_mediaAuto);
        }

        uint16_t keyPopupMs() { return g_keyPopupMs; }

        void keyPopupShown()
        {
            g_keyPopups.fetch_add(1, std::memory_order_relaxed);
            // Armed only on a successful post: never hide a popup we did not show.
            g_armPopupReq.post(g_keyPopupMs);
        }

        bool mediaAuto() { return g_mediaAuto; }

        Staged<MediaSettings>&               settings()    { return s_settings; }
        Staged<PaneBits>&                    paneBits()    { return s_paneBits; }
        const Snapshot<UiStatus>&            status()      { return s_status; }
        const Mirror<MegaOpen::MediaScreen>& mediaMirror() { return s_mediaMirror; }
        const Mirror<MegaOpen::MediaPane>&   paneMirror()  { return s_paneMirror; }

        Words words()
        {
            Words w;
            w.keyPopup    = &g_keyPopup;
            w.keyPopupMs  = &g_keyPopupMs;
            w.glassHoldMs = &g_glassHoldMs;
            return w;
        }
    }
}
#endif
