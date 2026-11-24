#include "KeyPath.h"
#include <atomic>
#include "KeyBus.h"
#include "HuLink.h"
#include "CarTask.h"
#include "Rt.h"
#include "TaskPlan.h"
#include "TaskRegistry.h"
#include "../board/BoardProfile.h"
#include "../cfg/CarStore.h"
#include "../input/MatrixScanner.h"
#include "../sinks/UartTx.h"
#include "../utils/Log.h"
#include "../utils/LogBus.h"
#include "../vehicle/Rules.h"
#if FEATURE_USB_HID
#include "../sinks/UsbKeySink.h"
#endif
#if FEATURE_BLE
#include "Media.h"
#endif
#if FEATURE_DISPLAY_CAN
#include "Media.h"
#include "PanelMenu.h"
#include "PanelOwner.h"
#include "UiTask.h"
#endif

namespace App
{
    namespace KeyPath
    {
        namespace
        {
#if FEATURE_USB_HID
            // Wired media keys to the head unit; registered with g_engine like the UART sink.
            static Sinks::UsbHidKeySink g_usbKeys;
#endif
            static Keys::KeyMap          g_map;
            static Input::MatrixScanner  g_scanner;
            static Keys::KeyEngine       g_engine;
            static Sinks::CanboxUartSink g_keySink;
#if FEATURE_RESISTIVE_KEYS
            // Analog twin of g_keySink: same KeyEngine and routing, different wire. Both run
            // at once, so which one the head unit answers is a page switch, not a rebuild.
            static Sinks::ResistiveMap     g_resMap;
            static Sinks::ResistiveKeySink g_resSink;
#endif

            static uint32_t g_ghosts = 0;
            static uint32_t g_encSteps = 0;

            // ---- what the web edits, and the task that applies it ------------------------
            // Staged<T> (docs/design/event-tasks.md §3.1): httpd writes the staged copy under
            // a mutex; the owner swaps it in with a try-take on its own schedule, so a web
            // request never writes into a table a key is being looked up in.
            //   routing, macros, stalk map  -> the keys task
            //   matrix map and timing       -> the scan task (the keys task without a matrix)
            static Staged<App::KeysTask::Routing>   g_routingCfg;
            static Staged<App::KeysTask::KeyMapCfg> g_keymapCfg;
            static Staged<Keys::MacroTable>         g_macroCfg;
#if FEATURE_RESISTIVE_KEYS
            // Asked of the keys task: release every ladder rung (a class mask changed, or
            // the page stopped a learn hold).
            static OneShot<uint8_t>                 g_resAllOffReq;
            // The ladder's ohms, rungs and timing, and the page's learn hold: the keys
            // task drives the rung pins, so the web stages and asks, never calls.
            static Staged<Sinks::ResistiveMap>      g_resMapCfg;
            static OneShot<App::KeysTask::ResLearn> g_resLearnReq;
#endif

            // The scan task's registry id (src/app/TaskRegistry.h).
            static int g_scanTaskId = -1;

#if FEATURE_DISPLAY_CAN
            // ---- the panel's keys -----------------------------------------------------------
            static Keys::AffaKeySource   g_affaKeys;
            static Keys::AffaMap         g_affaMap;      // stalk key -> action + head-unit code; keys task
            static Staged<Keys::AffaMap> g_affaCfg;      // ...as the web edits it

            // The last panel key and where it went, for the wiring check on the page
            // (/api/affamap?live=1). The keys task publishes; httpd copies.
            static Snapshot<Keys::PanelKeyLast> g_keyLast;
            static void publishKeyLast(const Keys::AffaKeySource::Report& r, bool uartSends);

            // One press, as a line for a 26-column row.
            //
            //     VolDown H > 0x02 PRESS      routed, and this is the byte on the UART
            //     SrcNext > 0x85 x1           a wheel detent: the second byte is a count
            //     VolUp > not routed          the class mask does not send it here
            //     Pause H > no action         the map row is deliberately empty
            //     key 0113 > unknown          a wire code the eight-key table has no name for
            //
            // The translation comes from CanboxUartSink::explain, never re-derived here.
            // Returns whether the UART sends it (false when unmapped), for publishKeyLast.
            static bool formatKeyLine(const Keys::AffaKeySource::Report& r, char* out, size_t cap)
            {
                char name[16];
                const uint8_t idx = Keys::AffaMap::indexOf(r.key);
                if (idx < Keys::AffaMap::KEYS)
                    snprintf(name, sizeof(name), "%s", Keys::AffaMap::keyName(idx));
                else
                    // Unnamed key: show the raw code.
                    snprintf(name, sizeof(name), "key %04X", (unsigned)r.key);

                const char* hold = r.hold ? " H" : "";

                if (!r.mapped) {
                    snprintf(out, cap, "%s%s > %s", name, hold,
                             r.known ? "no action" : "unknown");
                    return false;
                }

                // Same event shape AffaKeySource builds.
                Keys::Event ev;
                ev.action   = r.action;
                ev.srcId    = 3;
                ev.codeHint = r.code;
                ev.state    = Keys::isEncoder(r.action) ? Keys::State::Press
                            : r.asLong                  ? Keys::State::Long
                                                        : Keys::State::Press;

                const Sinks::CanboxUartSink::Explain x = g_keySink.explain(ev);
                if (!x.sends) {
                    snprintf(out, cap, "%s%s > %s", name, hold, x.why);
                    return false;
                }
                if (x.knob) {
                    snprintf(out, cap, "%s%s > %02X x%u", name, hold,
                             (unsigned)x.code, (unsigned)x.state);
                    return true;
                }
                static const char* const kState[] = {"REL", "PRESS", "LONG"};
                snprintf(out, cap, "%s%s > %02X %s", name, hold, (unsigned)x.code,
                         x.state <= 2 ? kState[x.state] : "?");
                return true;
            }

            // ---- the panel's keys, straight onto the KeyBus -----------------------------
            // The key callback runs inside poll() and must not block, so it only posts the
            // raw code to the KeyBus; the keys task maps it (AffaKeySource) and runs the
            // sinks. The time between poll() starts rides along: it is how long the key's
            // frame could have sat in the TWAI queue before the library saw it, which the
            // key latency line would otherwise hide (App::Panel::pollGapUs).
            //
            // Bound as AffaDisplayBase::KeyCb. Never waits: a full lane drops and counts.
            static void onPanelKey(affa::Key k, affa::KeyEdge e, void*)
            {
                App::KeyBus::postPanelKey((uint16_t)k, e == affa::KeyEdge::Hold, millis(),
                                          App::Panel::pollGapUs());
            }

            // Bound as AffaKeySource::ReportCb; runs on the keys task, inside the mapping.
            // Renders through PanelOps so every render goes through one path, and asks
            // the render owner (App::Ui) to arm the popup's take-down.
            static void onAffaKeyReport(const Keys::AffaKeySource::Report& r, void*)
            {
                char line[MegaOpen::PanelOps::STR_MAX];
                publishKeyLast(r, formatKeyLine(r, line, sizeof(line)));

                // Logged whether or not the popup is on, through the LogBus (it copies and
                // returns; see onKeyEvent). A small buffer: this is the keys task's stack.
                char b[MegaOpen::PanelOps::STR_MAX + 16];
                snprintf(b, sizeof(b), "[panelkey] %s", line);
                LogBus::line(b);

                // Track keys get no popup when the track splash will follow.
                const bool trackKey = r.action == Keys::Action::SrcNext || r.action == Keys::Action::SrcPrev ||
                                      r.action == Keys::Action::EncCw   || r.action == Keys::Action::EncCcw;
                if (!App::PanelMenu::popupAllowed(r.action)) return;
                if (!App::Ui::keyPopupWanted(trackKey)) return;
                // Armed only on a successful post: never hide a popup we did not show.
                if (App::Panel::ops().postPopup(line)) App::Ui::keyPopupShown();
            }

            // No fullscreen bookkeeping: on this panel every full-screen-class render (menu,
            // fullscreen, text) replaces the last one without a hide. Only the popup stays on
            // top until hidePopup() (02 54 03), hence its deadline.

#endif

#if FEATURE_BLE
            // On the keys task they only post; ble_svc performs (sinks/BleKeyProxy.h).
            static Sinks::BleKeyProxy g_amsKeys(Sinks::BleCmd::Target::Ams);
            static Sinks::BleKeyProxy g_hidKeys(Sinks::BleCmd::Target::Hid);

            // Whether each BLE key sink is in the dispatch chain; set in setup().
            static bool g_amsSinkLive = false;
            static bool g_hidSinkLive = false;
            // Volume falls back to AMS (keysVolumeFallback). Written by the keys task,
            // read by the web.
            static std::atomic<bool> g_volToAms{false};
#endif

#if FEATURE_DISPLAY_CAN
            // On the keys task, inside the mapping and ahead of the sinks it describes:
            // reads the sinks' own masks and switches (this task owns them), no allocation,
            // no log, and the only lock is the snapshot's microsecond copy, released
            // before any sink runs. `dests` is where the key is routed: mask, switched on,
            // and the sink's own verdict where it has one (UART: a code; ladder: a rung).
            // Not seen here: a macro that swallows the key, a phone or USB host that is
            // not connected (the BLE worker and the USB sink drop those later).
            static void publishKeyLast(const Keys::AffaKeySource::Report& r, bool uartSends)
            {
                Keys::PanelKeyLast k;
                k.wire   = (uint16_t)r.key;
                k.hold   = r.hold;
                k.key    = r.known ? Keys::AffaMap::indexOf(r.key) : 0xFF;
                k.action = (uint8_t)r.action;
                if (r.mapped) {
                    // The first edge AffaKeySource dispatches is a Long for an asLong row.
                    const bool isLong = r.asLong && !Keys::isEncoder(r.action);

                    Keys::DestState d[Keys::DEST_COUNT];
                    // Live only while the head unit is heard, as in /api/health keys.dest.
                    d[Keys::DEST_UART] = {g_keySink.classes(), uartSends && App::Media::hu().heard(millis()), false};
#if FEATURE_BLE
                    d[Keys::DEST_AMS] = {g_amsKeys.classes(), g_amsSinkLive, true};
                    d[Keys::DEST_HID] = {g_hidKeys.classes(), g_hidSinkLive, true};
#endif
#if FEATURE_RESISTIVE_KEYS
                    Keys::Event ev;
                    ev.action   = r.action;
                    ev.srcId    = 3;
                    ev.codeHint = r.code;
                    ev.state    = isLong ? Keys::State::Long : Keys::State::Press;
                    d[Keys::DEST_RES] = {g_resSink.classes(), g_resSink.explain(ev).acts, false};
#endif
#if FEATURE_USB_HID
                    d[Keys::DEST_USB] = {g_usbKeys.classes(), true, true};
#endif
                    k.dests = Keys::keyDests(r.action, isLong, d);
                }
                g_keyLast.publish(k);
            }
#endif

            // The UART key sink's write: the keys task, into the FIFO or UartTx's hold.
            // False = refused, so the sink keeps a release and retries it.
            static bool writeKeyFrame(const uint8_t* frame, uint8_t len, void*)
            {
                return App::HuLink::tx().writeKey(frame, len);
            }

            // ---- matrix GPIO ------------------------------------------------------------
            static void driveRow(uint8_t row, bool active, void*)
            {
                const int pin = Board::PIN_ROW[row];
                if (active) { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
                else        { pinMode(pin, INPUT); }        // Hi-Z, never driven high
            }

            static uint8_t readCols(void*)
            {
                uint8_t bits = 0;
                for (uint8_t c = 0; c < Board::MATRIX_COLS; ++c)
                    if (digitalRead(Board::PIN_COL[c]) == LOW) bits |= (uint8_t)(1u << c);
                return bits;
            }

            static void settle(void*) { delayMicroseconds(5); }

#if FEATURE_RESISTIVE_KEYS
            // ---- KEY1/KEY2 ladder GPIO --------------------------------------------------
            // A leg either pulls its resistor to ground or is Hi-Z. Never OUTPUT HIGH: it
            // would fight the head unit's pull-up into its ADC (and its 5 V through the
            // ESP32 clamp). Active-high = MOSFET build: the pin drives a gate, not KEY.
            static void resApply(uint8_t ch, uint8_t mask, void*)
            {
                const int8_t* pins = Board::resPins(ch);
                const uint8_t legs = Board::resLegs(ch);
                const bool activeHigh = g_resMap.activeHigh();

                for (uint8_t i = 0; i < legs; ++i) {
                    const int8_t pin = pins[i];
                    if (pin < 0) continue;
                    const bool on = (mask & (1u << i)) != 0;
                    if (activeHigh) {                       // gate of a 2N7002
                        pinMode(pin, OUTPUT);
                        digitalWrite(pin, on ? HIGH : LOW);
                    } else if (on) {                        // the pin IS the switch
                        pinMode(pin, OUTPUT);
                        digitalWrite(pin, LOW);
                    } else {
                        pinMode(pin, INPUT);                // Hi-Z — the switch is open
                    }
                }
            }
#endif

            // ---- observers --------------------------------------------------------------
            // Dispatched key events since boot, as a plain variable for /api/health.
            static uint32_t g_engineEvents = 0;

            // The engine's event hook: every event that reached the sinks, on the keys
            // task (the only one that dispatches now).
            static void onKeyEvent(const Keys::Event& e, void*)
            {
                ++g_engineEvents;
                // The down set: what the keys task may have to let go if a release is lost.
                App::KeysTask::observe(e);
                const bool enc = Keys::isEncoder(e.action);
                if (enc) ++g_encSteps;
                // One line per key event at DBG ("key").
                static int8_t s_keyTag = -1;
                if (!Log::on(LogLevel::DBG, "key", s_keyTag)) return;
                // Encoders: at most one line per 50 ms, carrying the steps folded into it
                // (g_encSteps has the exact count). A fast spin is hundreds of events a
                // second, and a full LogBus ring drops the [stall]/[task]/[hu] lines
                // instead.
                static uint32_t s_encLineMs = 0, s_encFolded = 0;
                if (enc) {
                    const uint32_t now = millis();
                    if (s_encLineMs && now - s_encLineMs < 50) { ++s_encFolded; return; }
                    s_encLineMs = now;
                }
                // Through the LogBus: this runs inline in every dispatch, and the LogBus
                // copies and returns.
                // A small buffer, not a 320-byte log line: the keys task's stack is 3 kB.
                char b[48];
                if (enc && s_encFolded) {
                    snprintf(b, sizeof(b), "D [key] %s %u +%u", Keys::name(e.action), (unsigned)e.state,
                             (unsigned)s_encFolded);
                    s_encFolded = 0;
                } else {
                    snprintf(b, sizeof(b), "D [key] %s %u", Keys::name(e.action), (unsigned)e.state);
                }
                LogBus::line(b);
            }

            // ---- macros -----------------------------------------------------------------
            // Matching lives in src/keys/MacroEngine.* (host tested); here is the executor.
            // All of it runs on the keys task, the engine's only caller, so there is no
            // lock: an effect on state another task owns is posted to that task.
            static Keys::MacroEngine  g_macros;

            // Held taps and synthetic releases. dispatchRaw, not dispatch: the filter must
            // not see its own output.
            static void macroEmit(const Keys::Event& e, void*) { g_engine.dispatchRaw(e); }

            // The panel menu first (App::PanelMenu): a key the menu takes is no macro's.
            static bool macroFilter(const Keys::Event& e, void*)
            {
#if FEATURE_DISPLAY_CAN
                if (App::PanelMenu::filter(e)) return true;
#endif
                return g_macros.observe(e);
            }



            // A gesture MacroEngine detected on the keys task: the car task checks the
            // rule's "only if" and pause and performs its actions. Keys::Macro::rule is
            // the row the macro stands for (Vehicle::Rules::gestures).
            static void macroFire(const Keys::Macro& m, void*)
            {
                if (m.rule != 0xFFFF) App::Car::fireRule(m.rule, m.gen);
            }

            // ---- the matrix map, staged ----------------------------------------------------
            // Applied where the map is read: the scan task when there is a matrix, else
            // the keys task (nothing sweeps).
            static void applyKeymapIfDirty()
            {
                if (!g_keymapCfg.applyIfDirty()) return;
                const App::KeysTask::KeyMapCfg& c = g_keymapCfg.live();
                g_map = c.map;
                g_engine.setEncoderInverted(c.encInverted);
                g_engine.applyMap();
                g_engine.setTiming(c.timing);
            }

            // ---- scan task --------------------------------------------------------------
            // 1 kHz. Sweeps the matrix and posts its edges to the KeyBus; the keys task
            // dispatches them, so a press waits for neither loop() nor a sink here.
            static void matrixEmit(const Keys::Event& e, void*)
            {
                App::KeyBus::postEvent(e);
            }

            static void scanTask(void*)
            {
                TickType_t last = xTaskGetTickCount();
                for (;;) {
                    const uint32_t now = millis();
                    const uint32_t t0 = micros();
                    applyKeymapIfDirty();
                    g_engine.poll(now);
                    g_ghosts = g_scanner.ghosts();
                    App::Tasks::beat(g_scanTaskId, micros() - t0);
                    vTaskDelayUntil(&last, pdMS_TO_TICKS(1));
                }
            }

            // ---- the keys task's hooks (src/app/KeysTask.h) --------------------------------
            // The task is generic; what it owns is wired here.

            // Staged web edits into the objects the keys task owns. Try-takes only.
            static void keysApplyRouting(const App::KeysTask::Routing& r)
            {
                g_keySink.setEnabled(r.uartOn);
                g_keySink.setClasses(r.uartCls);
                g_keySink.setPulseMs(r.pulseMs);
#if FEATURE_BLE
                // While volume falls back to the phone it is the phone's alone.
                if (g_volToAms.load()) g_keySink.setClasses((uint8_t)(r.uartCls & ~Keys::CLASS_VOLUME));
#endif
#if FEATURE_RESISTIVE_KEYS
                // Only on a change: switching the ladder off also opens every rung.
                if (g_resSink.enabled() != r.resOn) g_resSink.setEnabled(r.resOn);
                if (g_resSink.classes() != r.resCls) {
                    g_resSink.setClasses(r.resCls);
                    // A rung held down under a class just masked off would never see its
                    // Release.
                    g_resSink.allOff();
                }
#endif
#if FEATURE_BLE
                g_amsKeys.setClasses(r.amsCls | (g_volToAms.load() ? Keys::CLASS_VOLUME : 0));
                g_hidKeys.setClasses(r.hidCls);
#endif
#if FEATURE_USB_HID
                g_usbKeys.setClasses(r.usbCls);
#endif
            }

#if FEATURE_BLE
            // Volume nothing live would take goes to the phone, and only to it
            // (Keys::applyVolumeFallback; not in the first minute after boot): the
            // UART dest is live only while the head unit is heard, so a phone-only car,
            // or a head unit that went quiet, does not leave the volume keys dead. Every
            // keys pass (at most 100 ms apart); logged once per change, not per press.
            static void keysVolumeFallback()
            {
                const App::KeysTask::Routing& r = g_routingCfg.live();
                Keys::DestState d[Keys::DEST_COUNT];
                d[Keys::DEST_UART] = {r.uartCls, r.uartOn && App::Media::hu().heard(millis()), false};
                d[Keys::DEST_AMS]  = {r.amsCls, g_amsSinkLive, true};
                d[Keys::DEST_HID]  = {r.hidCls, g_hidSinkLive, true};
#if FEATURE_RESISTIVE_KEYS
                d[Keys::DEST_RES]  = {r.resCls, r.resOn, false};
#endif
#if FEATURE_USB_HID
                d[Keys::DEST_USB]  = {r.usbCls, true, true};
#endif
                const bool on = Keys::applyVolumeFallback(d, millis());
                if (on == g_volToAms.load()) return;
                g_volToAms.store(on);
                g_amsKeys.setClasses(d[Keys::DEST_AMS].mask);
                g_keySink.setClasses(d[Keys::DEST_UART].mask);
                if (on) LOGI("key", "vol -> ams (%s)", r.uartOn ? "hu silent" : "uart off");
                else    LOGI("key", "vol -> routing (ams no longer the fallback)");
            }
#endif

            static void keysApply()
            {
                if (g_routingCfg.applyIfDirty()) keysApplyRouting(g_routingCfg.live());
#if FEATURE_BLE
                keysVolumeFallback();
#endif
                if (g_macroCfg.applyIfDirty())   g_macros.setTable(g_macroCfg.live());
#if FEATURE_DISPLAY_CAN
                if (g_affaCfg.applyIfDirty())    g_affaMap = g_affaCfg.live();
#endif
#if !FEATURE_MATRIX
                applyKeymapIfDirty();
#endif
#if FEATURE_RESISTIVE_KEYS
                // A new map: let go of every rung first, so no held key resolves its
                // Release against a rung the edit moved.
                if (g_resMapCfg.applyIfDirty()) {
                    g_resSink.allOff();
                    g_resMap = g_resMapCfg.live();
                }
                uint8_t x;
                if (g_resAllOffReq.take(x)) g_resSink.allOff();     // also ends a learn hold
                App::KeysTask::ResLearn q;
                if (g_resLearnReq.take(q)) {
                    const uint32_t now = millis();
                    if (q.op == App::KeysTask::ResLearn::Op::Action) {
                        // The page checked the rung against the staged map; this is the
                        // map that is live now, which can only differ by a racing edit.
                        if (!g_resSink.learnAction((Keys::Action)q.action, q.ms, now))
                            LOGI("res", "learn %s: no rung now - the map changed",
                                           Keys::name((Keys::Action)q.action));
                    } else {
                        g_resSink.learnAssert(q.ch, q.mask, q.ms, now);
                    }
                }
#endif
            }

            static void keysDeadlines(uint32_t now)
            {
                g_keySink.tick(now);           // scheduled encoder releases
#if FEATURE_USB_HID
                g_usbKeys.service(now);
#endif
#if FEATURE_RESISTIVE_KEYS
                g_resSink.tick(now);           // every ladder deadline
#endif
                g_macros.tick(now);            // sequence windows; held taps replay here
            }

            static bool keysNextDue(uint32_t now, uint32_t& due)
            {
                bool any = false;
                auto earliest = [&](uint32_t d) {
                    if (!any || (int32_t)(d - due) < 0) due = d;
                    any = true;
                };
                uint32_t d;
                if (g_keySink.nextDue(d)) earliest(d);
                if (g_macros.nextDue(d))  earliest(d);
#if FEATURE_USB_HID
                if (g_usbKeys.nextDue(d)) earliest(d);
#endif
#if FEATURE_RESISTIVE_KEYS
                // The ladder keeps its own pulse, gap and stuck-key times; while anything
                // is engaged, queued or learning it is ticked every 5 ms (its shortest
                // step is 50 ms), and not at all when idle.
                for (uint8_t c = 0; c < Sinks::ResistiveMap::CHANNELS; ++c)
                    if (g_resSink.liveMask(c) || g_resSink.queued(c) || g_resSink.learning(c)) {
                        earliest(now + 5);
                        break;
                    }
#endif
                return any;
            }

            static void keysStart()
            {
                App::KeysTask::Hooks h;
                h.apply     = keysApply;
                h.deadlines = keysDeadlines;
                h.nextDue   = keysNextDue;
                h.route.dispatch = [](const Keys::Event& e, void*) { g_engine.dispatch(e); };
#if FEATURE_DISPLAY_CAN
                h.route.panelKey = [](uint16_t code, bool hold, uint32_t tMs, void*) {
                    g_affaKeys.onPanelKey(code, hold, tMs);
                };
#endif
                h.route.literal = [](uint8_t code, uint8_t state, bool release, void*) {
                    // With a release: scheduled MIN_GAP_MS after the press (IKeySink.h).
                    if (release) g_keySink.sendTap(code, state);
                    else         g_keySink.sendKey(code, state);
                };
                App::KeysTask::begin(h, &App::HuLink::tx());
            }
        }

        void loadMap(Cfg::Config& cfg) { cfg.loadKeyMap(g_map); }

#if FEATURE_DISPLAY_CAN
        void loadPanelMap(Cfg::Config& cfg)
        {
            cfg.loadAffaMap(g_affaMap);
            g_affaCfg.init(g_affaMap);
        }
#endif

        void stage(Cfg::Config& cfg)
        {
            {
                App::KeysTask::KeyMapCfg k;
                k.map         = g_map;
                k.encInverted = cfg.s().encInverted;
                k.timing      = cfg.s().timing;
                g_keymapCfg.init(k);
            }

#if FEATURE_MATRIX
            for (uint8_t c = 0; c < Board::MATRIX_COLS; ++c) pinMode(Board::PIN_COL[c], INPUT_PULLUP);
            for (uint8_t r = 0; r < Board::MATRIX_ROWS; ++r) pinMode(Board::PIN_ROW[r], INPUT);
#endif
        }

        void beginSinks(Cfg::Config& cfg)
        {
            g_scanner.begin(driveRow, readCols, settle, nullptr);
            g_keySink.begin(writeKeyFrame, nullptr);
            // The release rule needs "now": a detent that waited on the KeyBus still
            // gets MIN_GAP_MS before its release (keys/IKeySink.h).
            g_keySink.setClock([]() -> uint32_t { return millis(); });
            g_keySink.setEnabled(cfg.s().uartEnabled);
            g_keySink.setClasses(cfg.s().routeUart);
            g_keySink.setPulseMs(cfg.s().pulseMs);
            {
                App::KeysTask::Routing r;
                r.uartOn  = cfg.s().uartEnabled;
                r.uartCls = cfg.s().routeUart;
                r.pulseMs = cfg.s().pulseMs;
                r.resOn   = cfg.s().resEnabled;
                r.resCls  = cfg.s().routeRes;
                r.amsCls  = cfg.s().routeAms;
                r.hidCls  = cfg.s().routeHid;
                r.usbCls  = cfg.s().routeUsb;
                g_routingCfg.init(r);
            }
        }

        void beginEngine(Cfg::Config& cfg)
        {
            g_engine.begin(&g_scanner, &g_map);
            g_engine.setTiming(cfg.s().timing);
            g_engine.setEncoderInverted(cfg.s().encInverted);
            g_engine.addSink(&g_keySink);
#if FEATURE_USB_HID
            g_usbKeys.begin();
            g_usbKeys.setClasses(cfg.s().routeUsb);
            g_engine.addSink(&g_usbKeys);
            LOGI("usb", "HID consumer control on the native USB, classes=0x%02X",
                 (unsigned)g_usbKeys.classes());
#endif
#if FEATURE_RESISTIVE_KEYS
            // Load the ladder first: begin() opens both channels, clearing whatever
            // state a reset left the pins in (else the head unit sees a held button).
            cfg.loadResMap(g_resMap);
            g_resMapCfg.init(g_resMap);
            g_resSink.begin(resApply, nullptr, &g_resMap);
            g_resSink.setEnabled(cfg.s().resEnabled);
            g_resSink.setClasses(cfg.s().routeRes);
            g_engine.addSink(&g_resSink);
            {
                // Printed every boot: legs and rung values live in NVS.
                char b[96];
                int n = snprintf(b, sizeof(b), "ch0 legs=%u ch1 legs=%u drive=%s%s ohms:",
                                 (unsigned)Board::RES_LEGS_CH0, (unsigned)Board::RES_LEGS_CH1,
                                 g_resMap.activeHigh() ? "mosfet" : "gpio",
                                 cfg.s().resEnabled ? "" : " (DISABLED)");
                for (uint8_t i = 0; i < Board::RES_LEGS_CH0 && n < (int)sizeof(b) - 8; ++i)
                    n += snprintf(b + n, sizeof(b) - n, " %u", (unsigned)g_resMap.ladder(0).ohmsFor((uint8_t)(1u << i)));
                LOGI("res", "%s", b);
            }
#endif
            g_engine.onEvent(onKeyEvent, nullptr);
#if FEATURE_MATRIX
            // The scan task posts; only the keys task dispatches.
            g_engine.onEmit(matrixEmit, nullptr);
#endif

            // The gesture filter sees every event before any sink; only it may stop
            // one. Its table is the gesture rules of the car file (Rules::gestures);
            // httpd rebuilds it on POST /api/rules.
            {
                Keys::MacroTable t;
                Vehicle::RuleSet rs;
                CarStore::loadRules(rs);
                const uint8_t n = Vehicle::Rules::gestures(rs, t, Keys::MacroTable::COUNT);
                g_macros.setTable(t);
                g_macroCfg.init(t);
                LOGI("rule", "%u gesture rule(s) on the keys task", (unsigned)n);
            }
            g_macros.onFire(macroFire, nullptr);
            g_macros.onEmit(macroEmit, nullptr);
            g_engine.onFilter(macroFilter, nullptr);
        }

#if FEATURE_DISPLAY_CAN
        void attachPanel(affa::AffaDisplayBase& panel)
        {
            // Keys arrive through onPanelKey() with their decode time; the clock is
            // for AffaKeySource::handle() only. See AffaKeySource::NowFn.
            g_affaKeys.begin(&g_engine, &g_affaMap, []() -> uint32_t { return millis(); });
            g_affaKeys.onReport(&onAffaKeyReport, nullptr);
            // onPanelKey only posts to the KeyBus; the keys task maps and dispatches.
            panel.onKey(&onPanelKey, nullptr);
        }
#endif

        void start() { keysStart(); }

        void startScan()
        {
            {
                // The matrix is the whole input path in this build: critical.
                App::TaskSpec spec;
                spec.name     = "scan";
                spec.fn       = scanTask;
                spec.stack    = App::Plan::SCAN_STACK;
                spec.prio     = App::Plan::SCAN_PRIO;
                spec.core     = App::Plan::SCAN_CORE;
                spec.critical = true;
                spec.idOut    = &g_scanTaskId;
                App::spawn(spec);
            }
        }

        Keys::KeyEngine&        engine()       { return g_engine; }
        Keys::KeyMap&           map()          { return g_map; }
        Sinks::CanboxUartSink&  keySink()      { return g_keySink; }
        const uint32_t*         ghosts()       { return &g_ghosts; }
        const uint32_t*         encSteps()     { return &g_encSteps; }
        const uint32_t*         engineEvents() { return &g_engineEvents; }
        Staged<KeysTask::Routing>&   routingCfg() { return g_routingCfg; }
        Staged<KeysTask::KeyMapCfg>& keymapCfg()  { return g_keymapCfg; }
        Staged<Keys::MacroTable>&    macroCfg()   { return g_macroCfg; }
#if FEATURE_RESISTIVE_KEYS
        Sinks::ResistiveKeySink&     resSink()      { return g_resSink; }
        Staged<Sinks::ResistiveMap>& resMapCfg()    { return g_resMapCfg; }
        OneShot<uint8_t>&            resAllOffReq() { return g_resAllOffReq; }
        OneShot<KeysTask::ResLearn>& resLearnReq()  { return g_resLearnReq; }
#endif
#if FEATURE_BLE
        Sinks::BleKeyProxy& amsKeys() { return g_amsKeys; }
        Sinks::BleKeyProxy& hidKeys() { return g_hidKeys; }
        void setBleSinks(bool amsLive, bool hidLive)
        {
            g_amsSinkLive = amsLive;
            g_hidSinkLive = hidLive;
        }
        bool amsSinkLive() { return g_amsSinkLive; }
        bool hidSinkLive() { return g_hidSinkLive; }
        bool volToAms()    { return g_volToAms.load(); }
#endif
#if FEATURE_DISPLAY_CAN
        Keys::AffaMap&                affaMap()  { return g_affaMap; }
        Keys::AffaKeySource&          affaKeys() { return g_affaKeys; }
        Staged<Keys::AffaMap>&        affaCfg()  { return g_affaCfg; }
        Snapshot<Keys::PanelKeyLast>& keyLast()  { return g_keyLast; }
#endif
    }
}
