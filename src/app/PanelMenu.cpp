#include "PanelMenu.h"
#if FEATURE_DISPLAY_CAN
#include <atomic>
#include <stdio.h>
#include <string.h>
#include "../cfg/Config.h"
#include "../cfg/SettingsRegistry.h"
#include "../display/PanelOps.h"
#include "../ui/MenuEngine.h"
#include "../ui/MenuInput.h"
#include "../ui/SignalText.h"
#include "../utils/LogBus.h"
#include "../vehicle/VehicleState.h"

namespace App
{
    namespace PanelMenu
    {
        namespace
        {
            using MenuUi::Mode;
            using MenuUi::Verdict;

            Deps s_d;

            // Written by the keys task (open, close, power on: at once, so the
            // next key in the same burst is judged right) and by the ui task
            // (the menu closing itself, power off).
            std::atomic<uint8_t> s_mode{(uint8_t)Mode::Closed};
            Mode mode() { return (Mode)s_mode.load(std::memory_order_acquire); }
            // Every move is from one known mode: two tasks race on it, and a
            // blind store could undo the other's. "Power off" (ui) against a
            // closing hold (keys) once left Closed with the panel off, where
            // no knob press wakes it.
            bool moveMode(Mode from, Mode to)
            {
                uint8_t f = (uint8_t)from;
                return s_mode.compare_exchange_strong(f, (uint8_t)to, std::memory_order_acq_rel);
            }

            // ---- a byte ring, one writer and one reader ---------------------------
            template <uint8_t N>
            struct Ring {
                static_assert((N & (N - 1)) == 0, "a power of two");
                uint8_t              buf[N];
                std::atomic<uint8_t> head{0}, tail{0};
                uint32_t             drops = 0;     // the writer's count

                bool push(uint8_t v)
                {
                    const uint8_t h = head.load(std::memory_order_relaxed);
                    if ((uint8_t)(h - tail.load(std::memory_order_acquire)) >= N) { ++drops; return false; }
                    buf[h & (N - 1)] = v;
                    head.store((uint8_t)(h + 1), std::memory_order_release);
                    return true;
                }
                bool pop(uint8_t& v)
                {
                    const uint8_t t = tail.load(std::memory_order_relaxed);
                    if (t == head.load(std::memory_order_acquire)) return false;
                    v = buf[t & (N - 1)];
                    tail.store((uint8_t)(t + 1), std::memory_order_release);
                    return true;
                }
            };

            // keys task -> ui task: what the keys asked for.
            enum : uint8_t { K_OPEN = 1, K_NEXT, K_PREV, K_SELECT, K_CLOSE, K_POWER_ON };
            Ring<16> s_keys;

            // ui task -> net_svc: committed values, as (registry index, value).
            struct Commit { uint8_t index; int32_t value; };
            constexpr uint8_t COMMITS = 8;
            Commit               s_commit[COMMITS];
            std::atomic<uint8_t> s_cHead{0}, s_cTail{0};
            // net_svc -> ui task: a commit was applied, re-read the knobs.
            std::atomic<bool>    s_applied{false};

            // ---- keys task only ------------------------------------------------------
            MenuUi::InputFilter s_in;

            // ---- ui task only --------------------------------------------------------
            MenuUi::Engine  s_menu;
            Cfg::Model      s_model;
            // The menu reads this copy, never the live Settings: those belong
            // to whoever holds Cfg::Config's lock (httpd, net_svc). Taken with
            // trySnapshot() when the menu opens and whenever a save() bumped
            // the generation; a busy lock keeps the old copy for a pass.
            Cfg::Settings   s_snap;
            uint32_t        s_snapGen = 0;
            bool            s_snapOk = false;
            Knobs::Config   s_knobs;          // the menu's copy, for reading
            bool            s_haveKnobs = false;
            bool            s_dirty = false;
            bool            s_popupWas = false;
            uint32_t        s_retryAtMs = 0;  // a refused render, tried again
            uint32_t        s_refreshAtMs = 0;
            uint32_t        s_lastKeyMs = 0;  // the menu closes IDLE_MS after it
            MenuUi::Engine::Screen s_sent;    // what the glass shows

            constexpr uint32_t REFRESH_MS = 1000;   // live values, at most this often
            constexpr uint32_t RETRY_MS   = 100;
            // A menu left open holds the glass: no media screen, no phone
            // notification, no car alert. It closes itself after this long
            // without a key (an edit in progress is dropped).
            constexpr uint32_t IDLE_MS    = 45000;

            // ---- the car's signals, for "Car data" -----------------------------------
            const char* groupTag(void*, uint8_t g)
            {
                switch (g) {
                    case Vehicle::GRP_DRIVE:    return "Drive";
                    case Vehicle::GRP_BODY:     return "Body";
                    case Vehicle::GRP_LIGHTS:   return "Lights";
                    case Vehicle::GRP_TRIP:     return "Trip";
                    case Vehicle::GRP_CLIMATE:  return "Climate";
                    case Vehicle::GRP_RADAR:    return "Parking";
                    case Vehicle::GRP_TPMS:     return "Tyres";
                    case Vehicle::GRP_AUDIO:    return "Audio";
                    case Vehicle::GRP_SETTINGS: return "Car settings";
                    case Vehicle::GRP_ENGINE:   return "Engine";
                    case Vehicle::GRP_SAFETY:   return "Safety";
                    case Vehicle::GRP_DYNAMICS: return "Dynamics";
                    default: {
                        static char b[12];
                        snprintf(b, sizeof(b), "Group %u", (unsigned)g);
                        return b;
                    }
                }
            }

            uint8_t groups(void*) { return Vehicle::GRP_COUNT; }

            uint8_t signals(void*, uint8_t g, uint8_t* out, uint8_t cap)
            {
                uint8_t n = 0;
                for (uint16_t s = 0; s < Vehicle::SIG_COUNT && n < cap; ++s)
                    if (Vehicle::meta((uint8_t)s).group == g && s_d.car->has((uint8_t)s)) out[n++] = (uint8_t)s;
                return n;
            }

            // "eng_temp" ... "92C" (src/ui/SignalText.h); "--" past STALE_MS.
            void sigRow(void*, uint8_t s, char* out, size_t cap)
            {
                MenuUi::signalRow(out, cap, MenuUi::COLS - 1, Vehicle::meta(s), s_d.car->get(s),
                                  s_d.car->ageMs(s, millis()) <= MenuUi::STALE_MS, Vehicle::verified(s));
            }

            MenuUi::CarView s_carView;

            // True: s_snap now matches the stored settings.
            bool refreshSnap()
            {
                const uint32_t g = s_d.cfg->generation();
                if (s_snapOk && g == s_snapGen) return true;
                if (!s_d.cfg->trySnapshot(s_snap)) return false;
                s_snapGen = g;
                s_snapOk = true;
                return true;
            }

            void refreshKnobs()
            {
                s_haveKnobs = s_d.knobsGet && s_d.knobsGet(s_knobs);
                s_model.k = s_haveKnobs ? &s_knobs : nullptr;
            }

            uint8_t scrollByte(bool up, bool down)
            {
                namespace C = affa::carminat;
                return up && down ? C::kScrollBoth : up ? C::kScrollUp : down ? C::kScrollDown : C::kScrollNone;
            }

            bool sameScreen(const MenuUi::Engine::Screen& a, const MenuUi::Engine::Screen& b)
            {
                return !strcmp(a.header, b.header) && !strcmp(a.row[0], b.row[0]) &&
                       !strcmp(a.row[1], b.row[1]) && a.more_up == b.more_up && a.more_down == b.more_down;
            }

            void render(uint32_t now)
            {
                MenuUi::Engine::Screen s;
                s_menu.screen(s);
                MegaOpen::PanelOps::Req r;
                r.op = MegaOpen::PanelOps::Op::Menu;
                snprintf(r.a, sizeof(r.a), "%s", s.header);
                snprintf(r.b, sizeof(r.b), "%s", s.row[0]);
                snprintf(r.c, sizeof(r.c), "%s", s.row[1]);
                r.u16 = scrollByte(s.more_up, s.more_down);
                // The ui task holds the glass while the menu is open (isOpen()
                // is in its glassHeld()); a deliberate render would only start
                // a hold timer that means nothing here.
                r.deliberate = false;
                r.quiet = true;
                if (s_d.ops->submit(r)) {
                    s_sent = s;
                    s_dirty = false;
                } else {
                    s_retryAtMs = now + RETRY_MS;
                }
            }

            void commit(uint8_t index, int32_t value)
            {
                const uint8_t h = s_cHead.load(std::memory_order_relaxed);
                if ((uint8_t)(h - s_cTail.load(std::memory_order_acquire)) >= COMMITS) {
                    LogBus::line("[menu] commit queue full: change dropped");
                    return;
                }
                s_commit[h % COMMITS] = Commit{index, value};
                s_cHead.store((uint8_t)(h + 1), std::memory_order_release);
            }

            void closeMenu(const char* why)
            {
                s_menu.close();
                moveMode(Mode::Open, Mode::Closed);
                s_d.ops->postClear();
                LogBus::printf("[menu] closed%s", why);
            }

            void onCommand(uint8_t k, uint32_t now)
            {
                s_lastKeyMs = now;
                if (k == K_POWER_ON) {
                    s_menu.close();
                    s_d.ops->postPower(true);
                    // The media screen, the pane and the clock come back.
                    s_d.ops->postClear();
                    LogBus::line("[menu] panel on");
                    return;
                }
                if (k == K_OPEN) {
                    if (s_menu.isOpen()) return;
                    refreshSnap();
                    refreshKnobs();
                    s_menu.open();
                    s_dirty = true;
                    s_refreshAtMs = now + REFRESH_MS;
                    LogBus::line("[menu] open");
                    return;
                }
                if (!s_menu.isOpen()) return;

                MenuUi::Cmd c = MenuUi::Cmd::Close;
                switch (k) {
                    case K_NEXT:   c = MenuUi::Cmd::Next; break;
                    case K_PREV:   c = MenuUi::Cmd::Prev; break;
                    case K_SELECT: c = MenuUi::Cmd::Select; break;
                    default:       break;
                }
                switch (s_menu.key(c)) {
                    case MenuUi::Out::None:
                        break;
                    case MenuUi::Out::Redraw:
                        s_dirty = true;
                        break;
                    case MenuUi::Out::Commit: {
                        const int idx = Cfg::Registry::indexOf(s_menu.commitEntry()->id);
                        if (idx >= 0) commit((uint8_t)idx, s_menu.commitValue());
                        // Redrawn when net_svc has stored it (s_applied).
                        s_dirty = true;
                        break;
                    }
                    case MenuUi::Out::Closed:
                        closeMenu("");
                        break;
                    case MenuUi::Out::PowerOff:
                        // Only from Open: a hold that closed the menu in the
                        // meantime wins, and the panel stays on.
                        if (moveMode(Mode::Open, Mode::PanelOff)) {
                            s_d.ops->postPower(false);
                            LogBus::line("[menu] panel off: a knob press switches it on");
                        } else {
                            s_d.ops->postClear();
                            LogBus::line("[menu] closed before power off: the panel stays on");
                        }
                        break;
                }
            }
        }

        void begin(const Deps& d)
        {
            s_d = d;
            // setup(), before any task that writes Settings: no lock needed.
            s_snap = d.cfg->s();
            s_snapGen = d.cfg->generation();
            s_snapOk = true;
            s_model.s = &s_snap;
            refreshKnobs();
            if (d.car) {
                s_carView.groups = groups;
                s_carView.groupTag = groupTag;
                s_carView.signals = signals;
                s_carView.row = sigRow;
            }
            s_menu.begin(&s_model, d.car ? &s_carView : nullptr);
        }

        bool filter(const Keys::Event& e)
        {
            const Verdict v = s_in.on(e, mode());
            switch (v.what) {
                case Verdict::Do::Pass:
                    return false;
                case Verdict::Do::Swallow:
                    return true;
                case Verdict::Do::Open:
                    // A full ring undoes the move: Open with no menu behind it
                    // would freeze the glass, Closed with the panel dark
                    // would leave nothing to wake it.
                    if (moveMode(Mode::Closed, Mode::Open) && !s_keys.push(K_OPEN))
                        moveMode(Mode::Open, Mode::Closed);
                    return true;
                case Verdict::Do::PowerOn:
                    if (moveMode(Mode::PanelOff, Mode::Closed) && !s_keys.push(K_POWER_ON))
                        moveMode(Mode::Closed, Mode::PanelOff);
                    return true;
                case Verdict::Do::Command:
                    switch (v.cmd) {
                        case MenuUi::Cmd::Next:   s_keys.push(K_NEXT); break;
                        case MenuUi::Cmd::Prev:   s_keys.push(K_PREV); break;
                        case MenuUi::Cmd::Select: s_keys.push(K_SELECT); break;
                        case MenuUi::Cmd::Close:
                            if (moveMode(Mode::Open, Mode::Closed)) s_keys.push(K_CLOSE);
                            break;
                    }
                    return true;
            }
            return false;
        }

        bool isOpen()   { return mode() == Mode::Open; }
        bool panelOff() { return mode() == Mode::PanelOff; }

        bool popupAllowed(Keys::Action a)
        {
            return mode() != Mode::Open || Keys::isVolume(a) || a == Keys::Action::Mute;
        }

        void service(uint32_t now, bool ready, bool popupUp)
        {
            uint8_t k;
            while (s_keys.pop(k)) onCommand(k, now);
            if (!s_menu.isOpen()) { s_popupWas = popupUp; return; }
            if ((uint32_t)(now - s_lastKeyMs) >= IDLE_MS) { closeMenu(" (idle 45 s)"); return; }

            if (s_applied.exchange(false, std::memory_order_acq_rel)) {
                refreshKnobs();
                refreshSnap();                  // applyPending saved or touched
                s_dirty = true;
            }
            // A save from the menu or the web: read the new values. While the
            // lock is busy the copy is not marked fresh and is retried.
            if (s_d.cfg->generation() != s_snapGen && !s_menu.editing() && refreshSnap())
                s_dirty = true;
            // Live values and signals that came or went: redraw only what changed.
            if ((int32_t)(now - s_refreshAtMs) >= 0) {
                s_refreshAtMs = now + REFRESH_MS;
                if (!s_menu.editing()) s_menu.refresh();
                MenuUi::Engine::Screen s;
                s_menu.screen(s);
                if (!sameScreen(s, s_sent)) s_dirty = true;
            }
            // The popup covered the list; draw it again once it is gone.
            if (s_popupWas && !popupUp) s_dirty = true;
            s_popupWas = popupUp;

            if (s_dirty && ready && !popupUp && (int32_t)(now - s_retryAtMs) >= 0) render(now);
        }

        void applyPending()
        {
            for (;;) {
                const uint8_t t = s_cTail.load(std::memory_order_relaxed);
                if (t == s_cHead.load(std::memory_order_acquire)) return;
                // The settings lock, or the commit waits for the next pass: a
                // web request holds it for its whole handler, and net_svc must
                // not stall behind one.
                if (!s_d.cfg->tryLock()) return;
                struct Held { Cfg::Config* c; ~Held() { c->unlock(); } } held{s_d.cfg};
                const Commit c = s_commit[t % COMMITS];
                s_cTail.store((uint8_t)(t + 1), std::memory_order_release);

                const Cfg::SEntry& e = Cfg::Registry::at(c.index);
                Knobs::Config kc;
                const bool haveK = s_d.knobsGet && s_d.knobsGet(kc);
                Cfg::Model m{&s_d.cfg->s(), haveK ? &kc : nullptr};
                uint8_t ap = 0;
                if (!Cfg::Registry::set(m, e, c.value, &ap)) {
                    LogBus::printf("[menu] %s = %ld refused", e.id, (long)c.value);
                    continue;
                }
                if ((ap & Cfg::AP_KNOBS) && s_d.knobsSet) s_d.knobsSet(kc);
                if ((ap & Cfg::AP_UI) && s_d.stageUi) s_d.stageUi(s_d.cfg->s());
                if ((ap & Cfg::AP_MEDIA) && s_d.stageMedia) s_d.stageMedia(s_d.cfg->s());
                if (ap & ~Cfg::AP_KNOBS) s_d.cfg->save();
                else s_d.cfg->touch();          // volatile or knobs only: no save, still news
                char v[24];
                Cfg::Registry::formatValue(e, c.value, v, sizeof(v));
                LogBus::printf("[menu] %s = %s%s", e.id, v, (ap & Cfg::AP_REBOOT) ? " (after a reboot)" : "");
                s_applied.store(true, std::memory_order_release);
            }
        }
    }
}
#endif
