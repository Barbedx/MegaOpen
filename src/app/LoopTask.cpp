#include "LoopTask.h"
#include "BootLog.h"
#include "CarTask.h"
#include "KeyPath.h"
#include "Rt.h"
#include "TaskPlan.h"
#include "TaskRegistry.h"
#include "../board/BoardProfile.h"
#include "../utils/Log.h"
#include "../utils/LogBus.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
#include "../diag/CanBench.h"
#include "../display/AffaLogSink.h"
#include <core/WakePacer.h>
#include <driver/twai.h>
#endif
#if FEATURE_VEHICLE_CAN && !FEATURE_DISPLAY_CAN
#include "../bus/TwaiVehicleSource.h"
#endif
#if FEATURE_DISPLAY_CAN
#include "PanelOwner.h"
#include "StockRadio.h"
#include "UiTask.h"
#include "../display/AffaPort.h"
#include "../display/PanelRole.h"
#endif

using App::Boot::memMark;

namespace App
{
    namespace Loop
    {
        namespace
        {
            // The registry id and handle of this task (the [stk] line names it
            // from net_svc).
            TaskHandle_t g_loopTask = nullptr;
            int          g_loopTaskId = -1;

#if FEATURE_DISPLAY_CAN
            // ---- the OEM display --------------------------------------------------------
            // Protocol lives in AffaDisplay; here are its ports (src/display/AffaPort.*) and
            // the policy. The library owns sync, ACKs, pacing, watchdog and bus-off recovery.
            static MegaOpen::ArduinoClock g_affaClock;
#endif

#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
            // The TWAI follows the build: the panel's bus where there is a display
            // (FEATURE_DISPLAY_CAN), else the car's (env:direct). One driver either way,
            // AffaDisplay's link (the IDF TWAI driver, rxq 64), owned by this loop:
            //   panel — the display's poll() runs it: recovery, the TX gate, protocol.
            //   car   — hardware listen-only, no display (<AffaTwai.h>): cannot transmit
            //           or ACK, so it can never go bus-off or flood a car's bus with error
            //           frames. carLinkPump() drains it to the car task.
            static affa::TwaiLink g_twai;
#if FEATURE_DISPLAY_CAN
            // Who else is on the panel bus (display/PanelBusWatch.h): fed by the raw tap,
            // read by panelWatchStep() on this task and by /api/health. It reports; the
            // role decides (display/PanelRole.h, App::Panel::role()).
            static MegaOpen::PanelBusWatch g_panelWatch;
#endif
            static uint32_t  g_canRx = 0;
            // The library's log lines (link and panel) into the LogBus.
            static MegaOpen::AffaLogSink g_affaLog;

            // Listen-only (never transmits or ACKs, so never bus-off; on in a build with
            // no display) is Diag::CanBench's, which samples the controller on this
            // loop, the driver's owner.
#endif

#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN

            // Ring of the most recent raw frames, both directions: the heartbeat names a
            // flood's id from it.
            struct RawFrame { uint32_t id; uint8_t len; uint8_t data[8]; char dir; };
            static constexpr uint8_t kLastFrames = 24;
            static RawFrame g_lastFrames[kLastFrames];
            static volatile uint8_t g_lastHead = 0;
            // The writers run on the panel and car tasks: an entry is copied in under
            // this lock, a few dozen bytes, so no writer leaves a torn frame.
            static portMUX_TYPE g_lastMux = portMUX_INITIALIZER_UNLOCKED;

            // Writers: carTap (the car task) and affaTap (the panel task). Length clamped
            // to classic CAN.
            static void ringPut(char dir, uint32_t id, const uint8_t* data, uint8_t len)
            {
                RawFrame f;
                f.dir = dir;
                f.id  = id;
                f.len = len > 8 ? 8 : len;
                for (uint8_t i = 0; i < 8; ++i) f.data[i] = i < f.len ? data[i] : 0;
                portENTER_CRITICAL(&g_lastMux);
                g_lastFrames[g_lastHead] = f;
                g_lastHead = (uint8_t)((g_lastHead + 1) % kLastFrames);
                portEXIT_CRITICAL(&g_lastMux);
            }

            // Every car frame, on the car task (App::Car's tap), after the decode: the
            // frame ring, the counter and the web's raw stream. The sniffer table and the
            // recorder are the car task's own. CAN A and the TWAI on env:direct both
            // arrive here; the TWAI owner (this loop) only hands its frames over
            // (TwaiVehicleSource).
            void carTapImpl(const Frame& f)
            {
                ++g_canRx;
                ringPut('R', f.id, f.data, f.len);
            }

#endif

#if FEATURE_VEHICLE_CAN && !FEATURE_DISPLAY_CAN
            // The car's TWAI, with no display to poll it: drained to the car task, which
            // decodes (TwaiVehicleSource), and recovered here. At most the driver queue's
            // depth a pass, so a flood cannot hold the loop. A link that is down is asked
            // to recover once a second: a restart can take hundreds of ms.
            static void carLinkPump(uint32_t now)
            {
                affa::Frame in;
                for (uint8_t n = 0; n < 64 && g_twai.recv(in); ++n) {
                    Frame f;
                    f.id       = in.id;
                    f.len      = in.len > 8 ? 8 : in.len;
                    memcpy(f.data, in.data, f.len);
                    f.extended = in.ext;
                    f.source   = Frame::SRC_MM_CAN;
                    TwaiVehicleSource::push(f, nullptr);
                }
                g_twai.service(now);
                static uint32_t s_recoverAtMs = 0;
                if (!g_twai.healthy() && (int32_t)(now - s_recoverAtMs) >= 0) {
                    s_recoverAtMs = now + 1000;
                    (void)g_twai.recover();
                }
            }
#endif

#if FEATURE_DISPLAY_CAN
            // ---- the whole AFFA bus, both directions, to the USB console ---------------
            // The panel link's raw tap (TwaiLink::onRaw) sees every frame the driver
            // delivers and every one it accepts for transmit. It runs inside poll(), so it
            // only pushes to a ring; loop() prints (a 40-char Serial write is ~3.5 ms at
            // 115200, enough to miss an ISO-TP deadline).
            struct AffaTrace { uint32_t id; uint32_t ms; uint8_t len; uint8_t dir; uint8_t data[8]; };
            // 64 records (1.3 kB), allocated the first time TRC is on (affaTraceDrain()),
            // PSRAM where there is some; null until then, and affaTap skips the ring. The
            // tap and the drain both run on the loop task, so no lock.
            static AffaTrace*      g_affaTrace = nullptr;
            static volatile uint8_t  g_affaTraceHead = 0;
            static volatile uint32_t g_affaTraceCount = 0;
            static uint32_t          g_affaTracePrinted = 0;
            // Printing is gated on log level TRC (logLevel, /api/settings): 115200 baud is ~280 lines/s,
            // a live bus is far more, and a blocked loop stops draining the display link
            // (RX FIFO overrun, registration never latches).
#define AFFA_TRACE_ON() Log::enabled(LogLevel::TRC)

            static void affaTap(const affa::Frame& f, affa::Direction d, void*)
            {
                // Received frames only: what the rest of the bus says, never our own.
                if (d != affa::Direction::Tx) {
                    g_panelWatch.onRx(f.id, f.len, f.data, millis());
                    // The extension role: the stock radio's prompts (its code).
                    if (App::StockRadio::active()) App::StockRadio::onRx(f.id, f.data, f.len, millis());
                }
                // Feed the raw frame ring: the panel bus is not a car source.
                ringPut((d == affa::Direction::Tx) ? 'T' : 'R', f.id, f.data, f.len);

                // The per-ID table is the car task's (one writer) and holds the car's
                // bus: the panel's frames stay in the ring above.

                if (!g_affaTrace) return;
                AffaTrace& t = g_affaTrace[g_affaTraceHead];
                t.id  = f.id;
                t.ms  = millis();
                t.len = f.len > 8 ? 8 : f.len;
                t.dir = (uint8_t)d;
                for (uint8_t i = 0; i < 8; ++i) t.data[i] = i < t.len ? f.data[i] : 0;
                g_affaTraceHead = (uint8_t)((g_affaTraceHead + 1) % 64);
                ++g_affaTraceCount;
            }

            // Call from loop() only.
            static void affaTraceDrain()
            {
                if (!AFFA_TRACE_ON()) { g_affaTracePrinted = g_affaTraceCount; return; }
                if (!g_affaTrace) {
                    void* m = heap_caps_calloc(64, sizeof(AffaTrace), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (!m) m = heap_caps_calloc(64, sizeof(AffaTrace), MALLOC_CAP_8BIT);
                    g_affaTrace = (AffaTrace*)m;
                    g_affaTracePrinted = g_affaTraceCount;
                    return;
                }
                const uint32_t count = g_affaTraceCount;
                if (count - g_affaTracePrinted > 64) {
                    LOGI("affa", "... %u frames lost\n",
                                  (unsigned)(count - g_affaTracePrinted - 64));
                    g_affaTracePrinted = count - 64;
                }
                while (g_affaTracePrinted < count) {
                    const AffaTrace& t = g_affaTrace[(uint8_t)(g_affaTracePrinted % 64)];
                    ++g_affaTracePrinted;
                    char b[96];
                    int n = snprintf(b, sizeof(b), "[affa] %8u %s %03X [%u]",
                                     (unsigned)t.ms,
                                     t.dir == (uint8_t)affa::Direction::Tx ? "TX>" : "<RX",
                                     (unsigned)t.id, (unsigned)t.len);
                    for (uint8_t i = 0; i < t.len; ++i)
                        n += snprintf(b + n, sizeof(b) - n, " %02X", t.data[i]);
                    LogBus::line(b);
                }
            }
#endif

            // ---- loop timing ------------------------------------------------------------
            // Everything that touches the panel runs on this loop; keys no longer do (they
            // are the keys task's, and [key] times them), and since Step 4 neither does
            // deciding what to draw (the ui task's, timed by [ui]). A slow pass is a late
            // poll(). lap() charges the time since the previous lap to a named
            // section; the slowest pass per heartbeat window is reported with its worst
            // section, and any pass over STALL_MS is logged on the spot.
            static uint32_t    g_lapAtUs = 0, g_passAtUs = 0;
            static uint32_t    g_passWorstUs = 0;
            static const char* g_passWorst = "-";
            static uint32_t    g_loopWinMaxUs = 0;
            static const char* g_loopWinWhere = "-";
            static uint32_t    g_loopWinWhereUs = 0;
            static constexpr uint32_t STALL_MS = 250;

            static inline void lap(const char* section)
            {
                const uint32_t t = micros();
                const uint32_t d = t - g_lapAtUs;
                g_lapAtUs = t;
                if (d > g_passWorstUs) { g_passWorstUs = d; g_passWorst = section; }
            }

            // ---- [stall]: how long the loop went without running ------------------------
            // From the end of one pass to the start of the next, by esp_timer (64-bit,
            // immune to millis() granularity). The pass itself is not in it — that is what
            // [loop] times — so all that should be there is the wait for the next CAN
            // frame, at most 2 ms. Anything far above that is time this task was not given
            // the CPU at all: a flash erase freezing both caches (a log flush), or a
            // higher-priority task hogging core 1. [loop] cannot show either, because the
            // loop is not running to time it.
            static int64_t  g_loopDoneAtUs = 0;
            static uint32_t g_stallWinMaxUs = 0;
            static uint32_t g_stallWinAtMs = 0;

            // ---- the loop's own evidence, every 10 s, from its own windows ----------------
            // [can], [panel], [stall] and [loop] describe what this task owns, so this task
            // writes them and resets their windows: one writer per window, like [key] and
            // [hu]. Everything else in the heartbeat is net_svc's (hkBeat).
            static void logLoopWindow(uint32_t now)
            {
                static uint32_t s_at = 0;
                if ((uint32_t)(now - s_at) < 10000) return;
                s_at = now;
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
                // Controller state and error counters (no ACK drives TX errors to bus-off).
                // The sample Diag::CanBench took this 100 ms, not a second read.
                const Diag::CanStatus st = Diag::CanBench::status();
                if (st.installed) {
                    static const char* const kState[] = {"stopped", "running", "bus-off", "recovering"};
                    LOGS("can", "state=%s txerr=%u rxerr=%u txfail=%u rxmiss=%u buserr=%u "
                         "arblost=%u txq=%u rxq=%u",
                         kState[st.state <= 3 ? st.state : 0],
                         (unsigned)st.tec, (unsigned)st.rec,
                         (unsigned)st.txFailed, (unsigned)st.rxMissed,
                         (unsigned)st.busErr, (unsigned)st.arbLost,
                         (unsigned)st.msgsToTx, (unsigned)st.msgsToRx);

                    // Name a flood: an un-ACKed node retransmits one frame forever and
                    // rx_missed_count climbs. The dominant id comes from the frame ring.
                    {
                        static uint32_t s_missWas = 0, s_saidAt = 0;
                        const uint32_t miss = st.rxMissed;
                        const uint32_t d = miss - s_missWas;
                        s_missWas = miss;
                        if (d > 200 && (uint32_t)(now - s_saidAt) >= 10000) {
                            s_saidAt = now;
                            uint32_t top = 0; uint8_t best = 0;
                            for (uint8_t i = 0; i < kLastFrames; ++i) {
                                uint8_t n = 0;
                                for (uint8_t j = 0; j < kLastFrames; ++j)
                                    if (g_lastFrames[j].id == g_lastFrames[i].id) ++n;
                                if (n > best) { best = n; top = g_lastFrames[i].id; }
                            }
                            LOGI("can", "FLOOD: %u frames/s dropped, queue cannot keep up. "
                                 "%u of the last 24 are id %03X -- a node repeating one frame "
                                 "is a node nobody is ACKing: check termination (60R across "
                                 "CANH/CANL) and the transceiver's 3V3 under load",
                                 (unsigned)d, (unsigned)best, (unsigned)top);
                        }
                    }

                    // Reported only: the driver's owner recovers it (the display's poll()
                    // with a panel, carLinkPump() on the car's bus).
                    if (st.state == TWAI_STATE_BUS_OFF || st.state == TWAI_STATE_STOPPED)
                        LOGI("can", "%s (the driver's owner recovers it)", kState[st.state]);
                }
#endif
#if FEATURE_DISPLAY_CAN
                App::Panel::logWindow();
#endif
                // The L2 baseline (flash erases freezing both cores): read it during a
                // log rotate. at= is the uptime of the worst gap in this window.
                LOGS("stall", "loopGapMax=%u us at=%u ms", (unsigned)g_stallWinMaxUs,
                     (unsigned)g_stallWinAtMs);
                g_stallWinMaxUs = 0;
                g_stallWinAtMs = 0;
                // Slowest loop pass and its worst section: this window's maximum. Keys no
                // longer wait for it. The media source's mutex is the ui task's to wait
                // for now: its misses are in [ui] mediaBusy.
                LOGS("loop", "max=%u ms (%s %u ms)", (unsigned)(g_loopWinMaxUs / 1000),
                     g_loopWinWhere, (unsigned)(g_loopWinWhereUs / 1000));
                g_loopWinMaxUs = 0; g_loopWinWhere = "-"; g_loopWinWhereUs = 0;
            }

            // ---- the sleep between passes ---------------------------------------------------
            // Not delay(2) any more: the loop sleeps in twai_receive() (TwaiLink::waitRx), so
            // the frame that carries a panel key wakes it at once and goes to poll() in the
            // same pass (the link keeps it for the next recv()).
            //
            // Under a flood (an unanswered panel sends 4-10k frames/s) a frame is always
            // waiting, so the wait returns at once and the loop would never leave the CPU
            // at priority 5. affa::WakePacer polls at once on one early wake and yields a
            // tick on the next one back to back, so whatever runs below it on this core
            // (the ui task, IDLE1 and its watchdog) still gets time.
            static void loopWait()
            {
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
                static affa::WakePacer s_pacer;
                switch (s_pacer.after(g_twai.waitRx(2))) {
                    case affa::WakeAction::SleepPeriod:   vTaskDelay(pdMS_TO_TICKS(2)); break;
                    case affa::WakeAction::YieldThenPoll: vTaskDelay(1);                break;
                    case affa::WakeAction::PollNow:                                     break;
                }
#else
                // No CAN in this image: nothing to wake on but time.
                vTaskDelay(pdMS_TO_TICKS(2));
#endif
            }

#if FEATURE_DISPLAY_CAN
            // What the bus says about the role, on the panel task after each poll(): a
            // report only (health `panel` reads the watch itself; this logs each change)
            // and never a decision, the board does what its role says
            // (display/PanelRole.h). Extension: the stock radio's sync is the expected
            // bus, and its absence means nothing of ours shows. Emulator: another
            // master's sync means two radios fight over one panel.
            static void panelWatchStep(uint32_t now)
            {
                static bool     s_master = false, s_said = false;
                static uint32_t s_firstPoll = 0;
                if (!s_firstPoll) s_firstPoll = now ? now : 1;   // nothing could be heard before the first poll()
                const bool extension = App::Panel::role() == MegaOpen::PanelRole::Extension;
                const bool master    = g_panelWatch.foreignMaster(now);
                if (master != s_master) {
                    s_master = master;
                    s_said   = true;
                    const unsigned id   = (unsigned)g_panelWatch.masterId();
                    const unsigned gone = (unsigned)(MegaOpen::PanelBusWatch::GONE_MS / 1000);
                    if (extension) {
                        if (master) LOGI("panel", "the stock radio drives the panel bus (sync 0x%03X): drawing over it", id);
                        else        LOGW("panel", "no sync from the stock radio for %u s: nothing of ours shows until it is back", gone);
                    } else {
                        if (master) LOGW("panel", "another master syncs on the panel bus (0x%03X) while this board emulates the radio: "
                                                  "the two fight over the panel; with a stock radio fitted set the role to extension", id);
                        else        LOGI("panel", "the other bus master has been quiet for %u s", gone);
                    }
                    return;
                }
                // Extension with no radio heard at all: said once, GONE_MS after the first poll.
                if (extension && !s_said && (uint32_t)(now - s_firstPoll) >= MegaOpen::PanelBusWatch::GONE_MS) {
                    s_said = true;
                    LOGW("panel", "no stock radio heard on the panel bus since boot: in the extension role nothing of ours "
                                  "shows; with no stock radio fitted set the role to emulator");
                }
            }
#endif

            void passImpl()
            {
                const uint32_t now = millis();
                {
                    const int64_t t = esp_timer_get_time();
                    if (g_loopDoneAtUs) {
                        const int64_t gap = t - g_loopDoneAtUs;
                        const uint32_t gapUs = gap > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)gap;
                        if (gapUs > g_stallWinMaxUs) { g_stallWinMaxUs = gapUs; g_stallWinAtMs = now; }
                        // Named on the spot, like a slow pass: the window max says how bad,
                        // this says when, next to whatever else was logged at that moment.
                        if (gapUs / 1000 > STALL_MS)
                            LOGI("stall", "loop did not run for %u ms", (unsigned)(gapUs / 1000));
                    }
                }
                g_passAtUs = g_lapAtUs = micros();
                g_passWorstUs = 0;
                g_passWorst = "-";

#if FEATURE_VEHICLE_CAN
                // No display only; the display link drains itself inside AffaDisplay::poll().
#if !FEATURE_DISPLAY_CAN
                carLinkPump(now);
#endif
#endif

                // The head-unit UART (RX, acks, telemetry) is the hu task's, and every
                // key deadline the keys task's: neither waits for this loop any more.
#if FEATURE_DISPLAY_CAN
                affaTraceDrain();
#endif
                lap("trace");

#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
                // The controller is sampled every 100 ms.
                Diag::CanBench::service(now);
#endif
                lap("can");

#if FEATURE_DISPLAY_CAN
                // The display. poll() drains RX and delivers keys before pumping TX; all
                // its timing is wall-clock against IClock, so nothing here needs pacing.
                // What gets drawn was decided on the ui task and posted into the library;
                // its verdicts go back there from inside pump().
                if (App::Panel::present()) {
                    App::Panel::pump(now);
                    panelWatchStep(now);
                    // The extension role: the stock radio's code, when it asks.
                    if (App::StockRadio::active()) App::StockRadio::step(now);
                    lap("disp.poll");
                }
#endif
                lap("display");

                // Knob events go to the KeyBus from the knob task; every key deadline
                // (releases, ladder, USB HID, macro windows) runs on the keys task, the
                // emitter on the hu task, and the vehicle sources (CAN A, the
                // decode of env:direct's TWAI frames) on the car task.

                logLoopWindow(now);
                lap("heartbeat");
                // WiFi, the web, the heartbeat and the OTA verdict are net_svc's.

                // Charge this pass to the window, and name a stall the moment it happens.
                {
                    const uint32_t passUs = micros() - g_passAtUs;
                    if (passUs > g_loopWinMaxUs) {
                        g_loopWinMaxUs   = passUs;
                        g_loopWinWhere   = g_passWorst;
                        g_loopWinWhereUs = g_passWorstUs;
                    }
                    if (passUs / 1000 > STALL_MS)
                        LOGI("loop", "STALL %u ms, %u ms of it in %s", (unsigned)(passUs / 1000),
                             (unsigned)(g_passWorstUs / 1000), g_passWorst);
                    App::Tasks::beat(g_loopTaskId, passUs);
                }

                // [stall] starts counting here: the pass is over, only the sleep is left.
                g_loopDoneAtUs = esp_timer_get_time();
                loopWait();
            }
        }

        void registerTask()
        {
            // The registry, from its first entries. This task is the loop (setup()
            // runs on it) and it is critical: it owns the panel and the CAN driver,
            // and net_svc's OTA verdict reads its age. The LogBus is not: a stuck log
            // costs evidence, not keys.
            g_loopTaskId = App::Tasks::add("loop", true);
            g_loopTask = xTaskGetCurrentTaskHandle();
            App::Tasks::setHandle(g_loopTaskId, g_loopTask);
        }

        void begin(Cfg::Config& config)
        {
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
            LOGI("can", "rx=GPIO%d tx=GPIO%d", (int)Board::PIN_CAN_RX, (int)Board::PIN_CAN_TX);
#endif

#if FEATURE_VEHICLE_CAN && !FEATURE_DISPLAY_CAN
            // The car's bus: hardware listen-only, never transmits or ACKs. A swapped
            // pin pair is silence, not an error. Its frames go to the car task, which
            // decodes; this loop keeps the driver (carLinkPump).
            {
                affa::TwaiConfig cfg;
                cfg.tx         = (gpio_num_t)Board::PIN_CAN_TX;
                cfg.rx         = (gpio_num_t)Board::PIN_CAN_RX;
                cfg.listenOnly = true;
                // Before begin(), so the link's install lines reach the log.
                g_affaLog.begin([](const char* line) { LogBus::line(line); });
                affa::setLogSink(&g_affaLog);
                if (!g_twai.begin(cfg))
                    LOGI("can", "vehicle: TwaiLink::begin FAILED — no controller (it keeps trying)");
            }
            LOGI("can", "vehicle: listen-only, never transmits, never ACKs, "
                 "cannot go bus-off. No display on this bus.");
            LOGI("can", "no termination resistor needed here — an OBD tap is not a "
                 "two-node stub");
#endif

#if FEATURE_DISPLAY_CAN
            // Through the LogBus like every other line: Serial, the flash log and the
            // web log. Runs inside poll(): LogBus::line only
            // copies into its ring.
            g_affaLog.begin([](const char* line) { LogBus::line(line); });

            const auto& dt = config.s().displayType;
            // The role (display/PanelRole.h): the settings' word, fixed for this boot.
            // Config::begin() has already reduced an unknown id to the emulator.
            MegaOpen::PanelRole role = MegaOpen::PanelRole::Emulator;
            MegaOpen::PanelRoles::parse(config.s().panelRole.c_str(), role);
            {
                // `carminat`: the same object typed as CarminatDisplay (styled text,
                // message boxes, the 0x1F1 pane); null on an UpdateList panel. No RTTI
                // here. From here on the object is App::Panel's (setup() runs on the
                // loop task, which is the panel task) and PanelOps' for rendering.
                affa::CarminatDisplay* carminat = nullptr;
                affa::AffaDisplayBase* display =
                    MegaOpen::createDisplay(dt.c_str(), g_twai, g_affaClock, &carminat);
                App::Panel::ops().attach(display, carminat);
                // Callbacks before begin(), or the first poll()'s sync transition is
                // missed: App::Panel counts the verdicts, and every ticket's verdict
                // goes on to the ui task's UiQ.
                App::Panel::Setup ps;
                ps.display  = display;
                ps.carminat = carminat;
                ps.link     = &g_twai;
                ps.role     = role;
                ps.hooks.onCompletion = App::Ui::postCompletion;
                App::Panel::begin(ps);
            }
            affa::AffaDisplayBase& panel = App::Panel::lib();
            panel.setLogSink(&g_affaLog);

            // The library's TWAI link: a 64-deep RX queue (an unanswered panel sends
            // 4-10.7k frames/s), Busy/Rejected transmit, the TX gate and bus-off
            // recovery, all run from poll(). A swapped pin pair is silence, not an error.
            // After the log sink, so the link's own install lines reach the log.
            {
                affa::TwaiConfig cfg;
                cfg.tx = (gpio_num_t)Board::PIN_CAN_TX;
                cfg.rx = (gpio_num_t)Board::PIN_CAN_RX;
                const bool ok = g_twai.begin(cfg);
                if (ok) LOGI("can", "display: TwaiLink @500k normal, rxq=%u", (unsigned)cfg.rxQueue);
                else    LOGE("can", "display: TwaiLink::begin FAILED - no controller (it keeps trying)");
            }
            memMark("can+display");

            // The default, stated: the library powers the panel on during the opening.
            panel.setAutoPower(true);

            App::KeyPath::attachPanel(panel);

            // Every AFFA frame to USB serial, gated on log level TRC.
            g_twai.onRaw(affaTap, nullptr);
            LOGI("affa", "frame trace follows the log level - raise it to TRC (logLevel: the Service page or the panel menu); it will slow the loop");

            // begin() transmits nothing; the first heartbeat leaves on the first poll().
            panel.begin();
            memMark("display.begin");
            // The extension role's own work: the stock radio beside us (its code).
            if (role == MegaOpen::PanelRole::Extension) App::StockRadio::begin(config);

            LOGI("boot", "display=%s role=%s\n", dt.c_str(), MegaOpen::PanelRoles::id(role));
#endif

#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
            // The controller's status, sampled by this task between two passes.
            // Before httpd: its routes read it.
            {
                Diag::CanBench::Setup cb;
                // With the panel we ACK and transmit. On the car's bus: never.
                cb.listenOnly = !FEATURE_DISPLAY_CAN;
                cb.link = &g_twai;
                Diag::CanBench::begin(cb);
            }
#endif
            (void)config;
        }

        void beat() { App::Tasks::beat(g_loopTaskId, 0); }

        void start()
        {
            // The loop task is the panel task: the owner of AffaDisplay's poll() and of
            // the TWAI driver. It only pumps; what to draw is the ui task's. Priorities,
            // high to low (the table with the reasons is src/app/TaskPlan.h):
            //   7  keys (core 1)     — the one key dispatcher, microseconds a key
            //   6  hu (core 1)       — head-unit RX, acks, telemetry
            //   6  knobs, scan (core 1) — 1 kHz samplers, posting to the KeyBus
            //   5  loop (this task)  — panel protocol: poll(), TX gate
            //   2  ui (core 1)       — the render policy; renders through PanelOps
            //   2  httpd, ble_svc (core 0) — web requests; BLE services and AMS writes
            //   1  net_svc (core 0)  — web, WiFi, heartbeat, the OTA verdict
            //   1  logbus            — log output
            // It sleeps in twai_receive() between passes (loopWait), so a panel frame
            // wakes it and everything below it on this core runs while it waits.
            vTaskPrioritySet(nullptr, App::Plan::LOOP_PRIO);
            // setup() ran on this task for seconds without a beat; the first loop pass
            // must not open with "loop has not run for over 2 s".
            App::Tasks::beat(g_loopTaskId, 0);
        }

        void pass() { passImpl(); }

        TaskHandle_t task() { return g_loopTask; }
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
        const uint32_t* canRx() { return &g_canRx; }
        void carTap(const Frame& f) { carTapImpl(f); }
#else
        const uint32_t* canRx() { static const uint32_t s_none = 0; return &s_none; }
#endif
#if FEATURE_DISPLAY_CAN
        const MegaOpen::PanelBusWatch& panelWatch() { return g_panelWatch; }
#endif
    }
}
