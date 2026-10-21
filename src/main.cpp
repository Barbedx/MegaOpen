#include <Arduino.h>
#include <atomic>
#include "Features.h"
#include "Version.h"
#include "board/BoardProfile.h"
#include "cfg/Config.h"
#include "input/MatrixScanner.h"
#include "keys/KeyEngine.h"
#include "keys/KeyMap.h"
#include "keys/MacroEngine.h"
#include "sinks/CanboxUartSink.h"
#include "sinks/UartTx.h"
#include "proto/canbox/RaiseFramer.h"
#include "proto/canbox/HuDialect.h"
#include "display/PanelBusWatch.h"
#include "proto/canbox/Rav4Emitter.h"
// Knob codes: the second byte is a detent count, not a press state — no release.
#include "proto/canbox/Rav4Profile.h"
#include "vehicle/VehicleState.h"
#include "net/WebUi.h"
#include "net/WifiManager.h"
#include "net/IpNotice.h"
#include <WiFi.h>          // the heartbeat reports SSID/BSSID/gateway directly
#include "utils/Log.h"
#include "utils/LogBus.h"
#if FEATURE_LOGFS
#include "utils/LogFile.h"
#endif
#include <Preferences.h>
#include <esp_system.h>
#include <esp_timer.h>
#include "app/Rt.h"
#include "app/KeyBus.h"
#include "app/KeysTask.h"
#include "app/HuTask.h"
#include "app/OneShot.h"
#include "app/Housekeeping.h"
#include "app/CarTask.h"
#include "utils/Staged.h"
#include "utils/Snapshot.h"
#include <esp_heap_caps.h>

#if FEATURE_VEHICLE_CAN
#include "vehicle/CanSniffer.h"
#include "bus/TwaiVehicleSource.h"
#endif

#if FEATURE_RESISTIVE_KEYS
#include "sinks/ResistiveKeySink.h"
#endif

#if FEATURE_KNOBS
#include "knobs/KnobService.h"
#endif

#if FEATURE_CAN2
#include "bus/Mcp2518Source.h"
#endif
#if FEATURE_CAPTURE
#include "capture/CanRecorder.h"
#endif
#if FEATURE_USB_HID
#include "sinks/UsbKeySink.h"
// Wired media keys to the head unit; registered with g_engine like the UART sink.
static Sinks::UsbHidKeySink g_usbKeys;
#endif

#if FEATURE_BLE
#include "ble/BleHub.h"
#include "ble/HidRole.h"
#include "bluetooth.h"
#include "media/AmsMediaSource.h"
#include "sinks/BleKeyProxy.h"
#include "sinks/BleKeySinks.h"
#if FEATURE_ANCS
#include "apple_notification_service.h"
#endif
#include <time.h>
#endif

#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
#include "diag/CanBench.h"
#include "display/AffaLogSink.h"
#include <core/WakePacer.h>
#include <driver/twai.h>
#endif

#if FEATURE_DISPLAY_CAN
#include "display/AffaPort.h"
#include "display/PanelOps.h"
#include "display/MediaScreen.h"
#include "display/MediaPane.h"
#include "display/TextFold.h"
#include "keys/AffaMap.h"
#include "keys/AffaKeySource.h"
#include "keys/KeyDests.h"
#include "app/PanelOwner.h"
#include "app/UiTask.h"
#include "app/PanelMenu.h"
#endif

#if FEATURE_DISPLAY_CAN || FEATURE_BLE
#include "media/MediaRouter.h"
#endif
// Portable: its UTF-16 decoder also serves the exchange log in every build.
#include "media/HuMediaSource.h"
#include <ArduinoJson.h>
#include "app/OtaTrial.h"

// Wiring only — every decision lives in the objects below.

static Cfg::Config           g_cfg;
static Keys::KeyMap          g_map;
static Input::MatrixScanner  g_scanner;
static Keys::KeyEngine       g_engine;
static Sinks::CanboxUartSink g_keySink;
static UartTx                g_tx;
#if FEATURE_RESISTIVE_KEYS
// Analog twin of g_keySink: same KeyEngine and routing, different wire. Both run
// at once, so which one the head unit answers is a page switch, not a rebuild.
static Sinks::ResistiveMap     g_resMap;
static Sinks::ResistiveKeySink g_resSink;
#endif
#if FEATURE_KNOBS
// The two knobs and their LEDs. A source: events go through g_engine.dispatch().
static Knobs::Service        g_knobs;
#endif
static Vehicle::State        g_state;
static Canbox::Rav4Emitter   g_emitter;
static WebUi                 g_web;
static WifiManager           g_wifi;
static Canbox::RxParser      g_huRx;
// Which canbox profile the head unit speaks (proto/canbox/HuDialect.h).
static Canbox::HuDialect     g_huDialect;

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

// The LogBus sinks. They run on the LogBus task, so blocking here is allowed.
//
// Every line also stays in the LogBus ring, which is the web log
// (GET /api/logs?since=); these sinks decide Serial and the flash log.
//
// "[kev] " lines are the per-event key lines from onKeyEvent. Web log only: on
// Serial they would cost UART time per detent, and in the flash log a knob spin
// would turn into flash erases (which freeze both cores) at exactly the moment
// keys are being sent.
// "[hur] " lines are the head unit's frames, decoded (`R cmd=...`), web only
// for the same reason: the [hu] ring echo already puts every frame on Serial
// and in the flash log once.
//
// "[hu] <D> " lines (one capital letter, then a space) are that ring echo,
// one per frame: T to the head unit, V vehicle telemetry, R from it, J junk
// (huDrainSerial). On Serial, never in the flash log. The R echo is Serial only
// (LogBus::serialOnly): the web log has that frame decoded, as its [hur]
// line. No other [hu] line has that shape.
static constexpr char KEV[] = "[kev] ";
static constexpr char HUR[] = "[hur] ";
static constexpr char HUE[] = "[hu] ";
static bool isWebOnly(const char* line)
{
    return !strncmp(line, KEV, sizeof(KEV) - 1) || !strncmp(line, HUR, sizeof(HUR) - 1);
}
static bool isHuEcho(const char* line)
{
    constexpr size_t n = sizeof(HUE) - 1;
    return !strncmp(line, HUE, n) && line[n] >= 'A' && line[n] <= 'Z' && line[n + 1] == ' ';
}

// IDF/library lines from the esp_log tap (logTapVprintf). Where the IDF
// console is Serial itself (UART0: the DevKit) they are already on it, so the
// serial sink skips them; on a USB-CDC build the console is not Serial, and
// this is the only way they reach the USB port.
static constexpr char IDF[] = "[idf] ";
static bool isIdfLine(const char* line) { return !strncmp(line, IDF, sizeof(IDF) - 1); }

static void serialSink(const char* line, int len, uint32_t)
{
    if (isWebOnly(line)) return;
#if !ARDUINO_USB_CDC_ON_BOOT
    if (isIdfLine(line)) return;
#endif
#if ARDUINO_USB_CDC_ON_BOOT && !ARDUINO_USB_MODE
    // TinyUSB CDC (t2can_usbhid): its write() spins, with no timeout, while
    // the host holds DTR and does not read. Whole line or nothing.
    if (Serial.availableForWrite() < len + 1) return;
#endif
    Serial.write((const uint8_t*)line, len);
    Serial.write('\n');
}
#if FEATURE_LOGFS
// The [hu] ring echo never goes to flash, whatever the serial switch says: it
// is one line per head-unit frame, the volume that made the flash log erase
// sectors (both cores stopped) while driving. The web log and USB serial still
// show it, and /api/canbox/last keeps the recent frames. [panelkey] stays: one
// line per press, held in RAM with the rest (LogFile.h).
static void fileSink(const char* line, int len, uint32_t ms)
{
    if (!isWebOnly(line) && !isHuEcho(line)) LogFile::sink(line, len, ms);
}

// Every software reboot (ESP.restart, esp_restart, the OTA and rollback
// reboots): what is still in the LogBus ring and the flash log's RAM buffer
// goes to /log/0.log first. Not run on a panic or a power cut.
static void logShutdown()
{
    LogBus::line("[log] rebooting: log flushed");
#if FEATURE_CAPTURE
    // The capture's last chunk first: it is the one a drive was recorded for.
    Capture::flush(500);
#endif
    LogBus::flushAndWait(500);
}
#endif

// Formatted line into the LogBus: never blocks the caller.
static void logf(const char* fmt, ...)
{
    char b[LogBus::MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    LogBus::line(b);
}

// Task handles for the [stk] stack high-water line, which runs on net_svc
// and so must name the loop's own handle rather than ask for "this task".
static TaskHandle_t g_loopTask = nullptr;
static TaskHandle_t g_bleTask = nullptr;

// Task registry ids (src/app/TaskRegistry.h): each task beats its own, and the
// [task] heartbeat line and the OTA verdict read them all. -1 = not running.
static int g_loopTaskId = -1, g_bleTaskId = -1;
static int g_logbusTaskId = -1, g_scanTaskId = -1;

// Free heap at each setup() stage, with the delta since the last mark.
static void memMark(const char* what)
{
    static uint32_t s_last = 0;
    const uint32_t now = ESP.getFreeHeap();
    logf("[mem] %-12s free=%6u big=%6u %+d", what, (unsigned)now,
         (unsigned)ESP.getMaxAllocHeap(), s_last ? (int)now - (int)s_last : 0);
    s_last = now;
}

#if FEATURE_DISPLAY_CAN
// ---- the OEM display --------------------------------------------------------
// Protocol lives in AffaDisplay; here are its ports (src/display/AffaPort.*) and
// the policy. The library owns sync, ACKs, pacing, watchdog and bus-off recovery.
static MegaOpen::ArduinoClock g_affaClock;
static Keys::AffaKeySource   g_affaKeys;
static Keys::AffaMap         g_affaMap;      // stalk key -> action + head-unit code; keys task
static Staged<Keys::AffaMap> g_affaCfg;      // ...as the web edits it
// Every render, from any task (src/display/PanelOps.h). The library object
// itself is App::Panel's (src/app/PanelOwner.h), and the render state is the
// ui task's (src/app/UiTask.h).
static MegaOpen::PanelOps g_panelOps;

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
    if (g_panelOps.postPopup(line)) App::Ui::keyPopupShown();
}

// No fullscreen bookkeeping: on this panel every full-screen-class render (menu,
// fullscreen, text) replaces the last one without a hide. Only the popup stays on
// top until hidePopup() (02 54 03), hence its deadline.

#endif

#if FEATURE_VEHICLE_CAN
static Vehicle::CanSniffer g_sniffer;
#endif

#if FEATURE_DISPLAY_CAN || FEATURE_BLE
// Source-neutral now-playing: phone (AMS) and head unit (HuMediaSource);
// `mediaSource` in NVS picks which one reaches the glass.
static MediaRouter        g_media;
static HuMediaSource      g_hu;

// Only the router's active source reaches the screen. May run on the NimBLE host
// task, so it only sets a flag: MediaScreen is touched only by its owner (the
// ui task, App::Ui; a concurrent copy would let compose()'s strlen run past an
// unterminated title).
static void onMediaChange(IMediaSource& src, void*)
{
#if FEATURE_DISPLAY_CAN
    if (g_media.activeSource() == &src) App::Ui::mediaChanged();
#else
    (void)src;
#endif
}
#endif

#if FEATURE_BLE
static AmsMediaSource     g_ams;
// On the keys task they only post; ble_svc performs (sinks/BleKeyProxy.h).
static Sinks::BleKeyProxy g_amsKeys(Sinks::BleCmd::Target::Ams);
static Sinks::BleKeyProxy g_hidKeys(Sinks::BleCmd::Target::Hid);
static Sinks::BleKeyWorker g_bleKeys;

// Whether each BLE key sink is in the dispatch chain; set in setup().
static bool g_amsSinkLive = false;
static bool g_hidSinkLive = false;
// Volume falls back to AMS (keysVolumeFallback). Written by the keys task,
// read by the web.
static std::atomic<bool> g_volToAms{false};

// ---------------------------------------------------------------------------
// BLE service, on its own task
// ---------------------------------------------------------------------------
// ServiceAms() and ANCS Process() may block on GATT, and AffaDisplay's deadlines
// (ACK timeout, 5 s PEER_ALIVE watchdog) are wall clock; from loop() this stalled
// sync at 0x08 with 401/644 renders Timeout. A blocked task yields; a loop cannot.
// It also performs the keys' AMS writes and HID presses, which therefore share
// a task with ServiceAms and can no longer race it over the one client.
#endif  // FEATURE_BLE — the BLE globals and the note on their task

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
        d[Keys::DEST_UART] = {g_keySink.classes(), uartSends && g_hu.heard(millis()), false};
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

// ---------------------------------------------------------------------------
// Web servicing (ElegantOTA), on net_svc (src/app/Housekeeping.h)
// ---------------------------------------------------------------------------
// Not BLE-specific: must stay outside any FEATURE_BLE gate.
// The loop task owns the panel's deadlines, so the web's periodic work runs on
// net_svc instead: priority 1, on core 0, never in front of the panel or a key.
static void hkWeb(uint32_t now)
{
    (void)now;
    g_web.loop();
#if FEATURE_DISPLAY_CAN
    // Values chosen in the panel menu: stored here, with every other setting.
    App::PanelMenu::applyPending();
#endif
}

#if FEATURE_BLE
static void bleServiceTask(void*)
{
    bool amsPending = false;
    for (;;) {
        // Woken by a key for the phone or the head unit, or after 20 ms (the
        // old fixed sleep). While AMS steps are owed it does not sleep beyond
        // a tick: each write waits for its response, and that paces it. While
        // a HID release is owed, it wakes when that is due.
        uint32_t waitMs = amsPending ? 1 : 20;
        uint32_t holdLeft = 0;
        if (g_bleKeys.hidHoldLeft(millis(), holdLeft) && holdLeft < waitMs)
            waitMs = holdLeft ? holdLeft : 1;
        g_bleKeys.collect(waitMs);
        const uint32_t t0 = micros();
        // HID presses went out in collect(). Asked before the release below,
        // so a hold that ends in between still gets its release first.
        const bool hidOwed = g_bleKeys.hidHoldLeft(millis(), holdLeft);
        // The HID release, ahead of any AMS write: Service() does it again
        // first thing; that is a no-op.
        if (BleHub::hidActive()) Hid::service();
        // At most one AMS write, and none while a HID release is still owed:
        // the write holds this task for 30-130 ms, and the release would wait
        // behind it.
        amsPending = hidOwed ? g_bleKeys.pending() : g_bleKeys.step();
        // AMS's long read of a cut title holds this task for two GATT round
        // trips: not while a HID release is owed or key writes wait.
        Bluetooth::SetKeysBusy(g_bleKeys.hidHoldLeft(millis(), holdLeft) || g_bleKeys.pending());
        BleHub::Service();
        // busyMax here is how long GATT setup, ANCS and one AMS write can hold
        // this task.
        App::Tasks::beat(g_bleTaskId, micros() - t0);
    }
}
#endif

#if FEATURE_ANCS
// Runs on the NimBLE host task (GATT subscription callback): no display calls,
// nothing that blocks. Rows go to the ui task (App::Ui) under its spinlock.
static uint32_t g_ancsCount = 0;
static void onAncsNotification(const AppleNotificationService::NotificationInfo& n)
{
    ++g_ancsCount;

    // Always logged, in raw UTF-8.
    LogBus::printf("[ancs] %s | %s: %s",
                AppleNotificationService::CategoryName(n.categoryId),
                n.title.c_str(), n.message.c_str());

#if FEATURE_DISPLAY_CAN
    if (!g_cfg.s().ancsPopup) return;
    App::Ui::queueNotification(n.title.empty() ? AppleNotificationService::CategoryName(n.categoryId)
                                               : n.title.c_str(),
                               n.message.c_str());
#endif
}
#endif  // FEATURE_ANCS


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
// read by panelWatchStep() on this task and by /api/health.
static MegaOpen::PanelBusWatch g_panelWatch;
#endif
static uint32_t  g_canRx = 0;
// The library's log lines (link and panel) into the LogBus.
static MegaOpen::AffaLogSink g_affaLog;

// Listen-only (never transmits or ACKs, so never bus-off; on in a build with
// no display) is Diag::CanBench's, which samples the controller on this
// loop, the driver's owner.
#endif

// esp_log tap: IDF/library log lines into the LogBus as "[idf] ...", so they
// reach the flash log and the web (and USB serial where the IDF console is not
// Serial already; see serialSink). The console write itself is unchanged.
static int (*s_idfVprintf)(const char*, va_list) = nullptr;

// Bytes of stack left below this frame on the calling task.
static size_t stackLeft()
{
    const uint8_t* base = pxTaskGetStackStart(nullptr);
    const uint8_t* sp   = (const uint8_t*)__builtin_frame_address(0);
    return (base && sp > base) ? (size_t)(sp - base) : 0;
}

static int logTapVprintf(const char* fmt, va_list ap)
{
    // Lines logged by the LogBus task itself (a sink: LittleFS) are not fed
    // back into the LogBus: the console only. That is the
    // whole re-entry guard: LogBus::line never logs and never blocks, so no
    // other task can loop through here. (The shared s_tapOwner it replaces was
    // one global for every task, and any logging task cleared another's claim.)
    // Also skipped on a task without the room for the buffer plus vsnprintf
    // (NimBLE's host task runs this too).
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (self != LogBus::task() && stackLeft() > 1536) {
        char b[160];
        memcpy(b, IDF, sizeof(IDF) - 1);
        constexpr int P = (int)sizeof(IDF) - 1;
        va_list copy;
        va_copy(copy, ap);
        int n = vsnprintf(b + P, sizeof(b) - P, fmt, copy);
        va_end(copy);
        if (n > (int)sizeof(b) - P - 1) n = (int)sizeof(b) - P - 1;
        // Strip the IDF's trailing newline.
        while (n > 0 && (b[P + n - 1] == '\n' || b[P + n - 1] == '\r')) b[P + --n] = '\0';
        if (n > 0) LogBus::line(b);
    }
    return s_idfVprintf ? s_idfVprintf(fmt, ap) : vprintf(fmt, ap);
}

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
static void carTap(const Frame& f)
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

// ---- head-unit UART ---------------------------------------------------------
// A ring of recent head-unit frames, so traffic is kept without a page open
// (track-change text, 0x90 "send me command X" requests). 24 entries.
namespace {
    struct HuEntry {
        char     dir;            // 'T' ours, 'R' a framed reply, 'J' junk
        uint8_t  cmd;
        uint8_t  len;            // bytes stored, not bytes seen
        uint16_t total;          // bytes the frame declared, so truncation shows
        uint32_t atMs;
        // 64: the head unit's now-playing text is UTF-16LE (2 bytes a char).
        // `total` keeps the declared length, so truncation stays visible.
        uint8_t  data[64];
    };
    constexpr uint8_t kHuRingLen = 24;
    HuEntry  g_huRing[kHuRingLen] = {};
    uint8_t  g_huHead = 0;
    uint32_t g_huCount = 0;
    // The head unit's own frames (R, J) again, apart: ours go out every 200 ms
    // and push its rare ones (track text, 0x90, the clock) out of the shared
    // ring within seconds. The web's exchange log reads this one.
    constexpr uint8_t kHuInLen = 12;
    HuEntry  g_huIn[kHuInLen] = {};
    uint8_t  g_huInHead = 0;
    // Two writers: the keys task (its key frames, through uartTap) and the hu
    // task (everything else). An entry is ~80 bytes; the copy under the
    // spinlock is a few microseconds. Readers copy an entry out the same way.
    portMUX_TYPE g_huMux = portMUX_INITIALIZER_UNLOCKED;

    uint32_t g_huPrinted = 0;

    void huRecord(char dir, uint8_t cmd, const uint8_t* p, uint8_t len, uint16_t total)
    {
        const uint8_t n = len > sizeof(HuEntry::data) ? (uint8_t)sizeof(HuEntry::data) : len;
        const uint32_t now = millis();
        portENTER_CRITICAL(&g_huMux);
        HuEntry& e = g_huRing[g_huHead];
        e.dir = dir;
        e.cmd = cmd;
        e.total = total;
        e.atMs = now;
        e.len = n;
        memcpy(e.data, p, n);
        g_huHead = (uint8_t)((g_huHead + 1) % kHuRingLen);
        ++g_huCount;
        if (dir == 'R' || dir == 'J') {
            g_huIn[g_huInHead] = e;
            g_huInHead = (uint8_t)((g_huInHead + 1) % kHuInLen);
        }
        portEXIT_CRITICAL(&g_huMux);
    }

    // One entry, copied whole: a writer on the other task cannot tear it.
    void huEntryAt(uint8_t idx, HuEntry& out)
    {
        portENTER_CRITICAL(&g_huMux);
        out = g_huRing[idx % kHuRingLen];
        portEXIT_CRITICAL(&g_huMux);
    }
    // i-th newest of the head unit's own frames.
    void huInNewest(uint8_t i, HuEntry& out)
    {
        portENTER_CRITICAL(&g_huMux);
        out = g_huIn[(uint8_t)((g_huInHead + kHuInLen - 1 - i) % kHuInLen)];
        portEXIT_CRITICAL(&g_huMux);
    }
}

// The readable column; decodes ASCII-range UTF-16LE (the head unit's track text)
// when one byte parity is all zero, trying header offsets 0-3. Returns true when
// it decoded wide characters.
static bool huTextColumn(const uint8_t* d, uint8_t len, char* out, size_t cap)
{
    size_t n = 0;
    auto emit = [&](uint8_t c) {
        if (n + 1 < cap) out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    };

    // Strip trailing NUL padding first: the 0x99 text field is a fixed 32 bytes,
    // and zeroes satisfy both parities.
    uint8_t end = len;
    while (end && !d[end - 1]) --end;

    // A header shifts the parity: 0x99 carries `83 <field>` before the text.
    for (uint8_t hdr = 0; hdr <= 3 && (uint8_t)(hdr + 4) <= end; ++hdr) {
        bool zeros = true, anyText = false;
        // NUL half all zero; the other half has at least one printable char.
        for (uint8_t i = (uint8_t)(hdr + 1); i < end; i = (uint8_t)(i + 2))
            if (d[i]) { zeros = false; break; }
        for (uint8_t i = hdr; i < end; i = (uint8_t)(i + 2))
            if (d[i] >= 0x20 && d[i] < 0x7F) { anyText = true; break; }
        if (!zeros || !anyText) continue;
        for (uint8_t i = hdr; i < end; i = (uint8_t)(i + 2)) emit(d[i]);
        out[n] = '\0';
        return true;
    }

    for (uint8_t i = 0; i < len; ++i) emit(d[i]);
    out[n] = '\0';
    return false;
}

// Print what was recorded since the last pass. The hu task only. Each line is
// `[hu] <dir> ...` (see isHuEcho): the sinks route it by that shape. Prints the
// canonical frame `2E <cmd> <len> ... <cs>` (checksum recomputed) to match the
// head unit's CANBUS Info screen; a truncated entry prints its payload and says
// so. At most HU_ECHO_PER_PASS lines a pass: a burst of head-unit traffic is
// caught up over the next passes (or counted as lost), never formatted all at
// once in front of an ack.
static constexpr uint8_t HU_ECHO_PER_PASS = 4;
static void huDrainSerial()
{
    const uint32_t count = g_huCount;
    // Report frames lost when the console falls behind the ring.
    if (count - g_huPrinted > kHuRingLen) {
        LogBus::printf("[hu] ... %u frames lost (console behind the ring)",
                       (unsigned)(count - g_huPrinted - kHuRingLen));
        g_huPrinted = count - kHuRingLen;
    }
    for (uint8_t lines = 0; g_huPrinted < count && lines < HU_ECHO_PER_PASS; ++lines) {
        // Static: the hu task's stack is 3 kB, and this is its only caller.
        static HuEntry e;
        huEntryAt((uint8_t)(g_huPrinted % kHuRingLen), e);
        ++g_huPrinted;
        if (!e.dir) continue;

        // Sized for 64 payload bytes (192 hex chars) plus the text column.
        static char b[400];
        int n = snprintf(b, sizeof(b), "[hu] %c ", e.dir);
        if (e.dir == 'J') {
            // Junk: bytes outside any framing, printed raw.
            n += snprintf(b + n, sizeof(b) - n, "junk  ");
            for (uint8_t k = 0; k < e.len && n < (int)sizeof(b) - 4; ++k)
                n += snprintf(b + n, sizeof(b) - n, "%02X ", e.data[k]);
        } else if (e.len == e.total) {
            uint8_t sum = (uint8_t)(e.cmd + e.total);
            for (uint8_t k = 0; k < e.len; ++k) sum = (uint8_t)(sum + e.data[k]);
            n += snprintf(b + n, sizeof(b) - n, "2E %02X %02X ", e.cmd, e.total);
            for (uint8_t k = 0; k < e.len && n < (int)sizeof(b) - 8; ++k)
                n += snprintf(b + n, sizeof(b) - n, "%02X ", e.data[k]);
            n += snprintf(b + n, sizeof(b) - n, "%02X", (uint8_t)(sum ^ 0xFF));
        } else {
            n += snprintf(b + n, sizeof(b) - n, "cmd=%02X len=%u (stored %u) ",
                          e.cmd, (unsigned)e.total, (unsigned)e.len);
            for (uint8_t k = 0; k < e.len && n < (int)sizeof(b) - 4; ++k)
                n += snprintf(b + n, sizeof(b) - n, "%02X ", e.data[k]);
        }

        // The text column.
        if (n < (int)sizeof(b) - 80 && e.len) {
            char txt[72];
            const bool wide = huTextColumn(e.data, e.len, txt, sizeof(txt));
            // "u16" marks a UTF-16LE decode.
            snprintf(b + n, sizeof(b) - n, "  %s|%s|", wide ? "u16" : "", txt);
        }
        LogBus::line(b);
    }
}

// Newest first, same as the CAN ring. `inOnly`: the head unit's frames alone.
static String huLastFramesJson(bool inOnly)
{
    String out = "{\"total\":";
    out += (uint32_t)g_huCount;
    out += ",\"baud\":";
    out += (uint32_t)g_cfg.s().uartBaud;
    out += ",\"frames\":[";
    const uint8_t head = g_huHead;
    const uint32_t now = millis();
    const uint8_t rows = inOnly ? kHuInLen : kHuRingLen;
    for (uint8_t i = 0; i < rows; ++i) {
        HuEntry e;
        if (inOnly) huInNewest(i, e);
        else        huEntryAt((uint8_t)((head + kHuRingLen - 1 - i) % kHuRingLen), e);
        if (!e.dir) continue;
        char b[420];
        int n = snprintf(b, sizeof(b),
                         "%s{\"dir\":\"%c\",\"cmd\":\"%02X\",\"len\":%u,\"total\":%u,\"ageMs\":%u,\"data\":\"",
                         out.endsWith("[") ? "" : ",", e.dir, (unsigned)e.cmd,
                         (unsigned)e.len, (unsigned)e.total,
                         (unsigned)(now - e.atMs));
        for (uint8_t k = 0; k < e.len; ++k)
            n += snprintf(b + n, sizeof(b) - n, "%02X", e.data[k]);
        // The text column, decoded here so the browser need not.

        n += snprintf(b + n, sizeof(b) - n, "\",\"ascii\":\"");
        {
            char txt[72];
            const bool wide = huTextColumn(e.data, e.len, txt, sizeof(txt));
            for (const char* s = txt; *s && n < (int)sizeof(b) - 12; ++s) {
                // JSON-escape " and \.
                if (*s == '"' || *s == '\\') { b[n++] = '\\'; b[n++] = *s; }
                else b[n++] = *s;
            }
            b[n] = '\0';
            snprintf(b + n, sizeof(b) - n, "\",\"u16\":%s", wide ? "true" : "false");
        }
        out += b;
        // The track text as the media source reads it, UTF-8 (Cyrillic and all).
        if (e.dir == 'R' && e.len >= 4 && e.data[0] == HuMediaSource::MARKER_TEXT &&
            e.data[1] != HuMediaSource::FIELD_TRACK) {
            char t[HuMediaSource::TEXT_MAX];
            if (HuMediaSource::decodeWide(e.data + 2, (uint8_t)(e.len - 2), t, sizeof(t))) {
                JsonDocument j;
                j.set(t);
                out += ",\"field\":";
                out += (unsigned)e.data[1];
                out += ",\"text\":";
                serializeJson(j, out);
            }
        }
        out += "}";
    }
    out += "]}";
    return out;
}

// Every frame UartTx wrote, on the task that wrote it (keys or hu): into the
// ring, and nothing more. The log sees it as the hu task's [hu] echo.
static void uartTap(char dir, const uint8_t* frame, uint8_t len, void*)
{
    // A Raise frame is `2E <cmd> <len> <payload...> <checksum>`; store the payload
    // unwrapped, like the inbound rows from RxParser.
    if (len >= 4) huRecord(dir, frame[1], frame + 3, (uint8_t)(len - 4), frame[2]);
    else          huRecord(dir, 0, frame, len, len);
}

// The UART key sink's write: the keys task, into the FIFO or UartTx's hold.
// False = refused, so the sink keeps a release and retries it.
static bool writeKeyFrame(const uint8_t* frame, uint8_t len, void*)
{
    return g_tx.writeKey(frame, len);
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
    // A small buffer, not logf()'s 320 bytes: the keys task's stack is 3 kB.
    char b[48];
    if (enc && s_encFolded) {
        snprintf(b, sizeof(b), "%s%s %u +%u", KEV, Keys::name(e.action), (unsigned)e.state,
                 (unsigned)s_encFolded);
        s_encFolded = 0;
    } else {
        snprintf(b, sizeof(b), "%s%s %u", KEV, Keys::name(e.action), (unsigned)e.state);
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

#if FEATURE_DISPLAY_CAN
// `arg` carries up to three panel rows separated by '|'.
static void macroRows(const char* arg, char* a, char* b, char* c, size_t cap)
{
    a[0] = b[0] = c[0] = '\0';
    char* out[3] = { a, b, c };
    uint8_t row = 0, n = 0;
    for (const char* p = arg; *p && row < 3; ++p) {
        if (*p == '|') { out[row][n] = '\0'; ++row; n = 0; continue; }
        if (n < cap - 1) out[row][n++] = *p;
    }
    if (row < 3) out[row][n] = '\0';
}
#endif

// One macro, performed, on the keys task. Each effect goes where
// Keys::effectRoute() says: inline only for what this task owns (dispatch and
// the UART key sink); the rest is a post. Effects this build lacks are logged.
static void macroFire(const Keys::Macro& m, void*)
{
    const char* label = m.name[0] ? m.name : Keys::effectName(m.effect);
    LogBus::printf("[macro] %s", label);

    // By route: Keys::effectRoute() is the table test_macro checks, so where
    // each effect is performed is decided there, not only here.
    switch (Keys::effectRoute(m.effect)) {
        case Keys::EffectRoute::Keys:
            if (m.effect == Keys::Effect::Action) {
                // Through the routing matrix, like a real button.
                Keys::Event e;
                e.action   = m.action;
                e.state    = Keys::State::Press;
                e.tMs      = millis();
                e.srcId    = 3;                 // 3 = macro; 0 matrix, 1 encoder, 2 web
                e.codeHint = m.code;            // 0 = let the sink use its own profile
                g_engine.dispatchRaw(e);
                if (!Keys::isEncoder(m.action)) {
                    e.state = Keys::State::Release;
                    g_engine.dispatchRaw(e);
                }
            } else {
                // KeyCode: one literal code down the UART sink. Its release is
                // scheduled MIN_GAP_MS later, not sent on the press's heels;
                // a knob code carries a detent count and gets none.
                g_keySink.sendTap(m.code, Canbox::KEY_PRESS);
            }
            break;
        case Keys::EffectRoute::UartOut: {
            uint8_t frame[Canbox::MAX_FRAME];
            const uint8_t len = Canbox::buildFrame(m.code, m.data, m.dataLen, frame);
            // The hu task's vehicle lane: an arbitrary frame is not a key, and
            // must not go in front of the next ack.
            if (!len)
                LogBus::line("[macro] payload too long");
            else if (!App::Hu::post(App::Hu::Kind::Raw, frame, len))
                LogBus::line("[macro] uart queue full");
            break;
        }
        case Keys::EffectRoute::Hu:
            // Message: the hu task owns the emitter; it reports one with no data.
            App::Hu::requestSendNow(m.code);
            break;
#if FEATURE_DISPLAY_CAN
        case Keys::EffectRoute::Panel:
            if (m.effect == Keys::Effect::Text) {
                char a[MegaOpen::PanelOps::STR_MAX];
                char b[MegaOpen::PanelOps::STR_MAX];
                char c[MegaOpen::PanelOps::STR_MAX];
                macroRows(m.arg, a, b, c, sizeof(a));
                if (b[0] || c[0]) g_panelOps.postFullscreen(a, b, c);
                else              g_panelOps.postText(a);
            } else {
                g_panelOps.postPopup(m.arg);
                // Auto-hide, armed by the render owner: the deadline is its state.
                App::Ui::armPopup(App::Ui::keyPopupMs());
            }
            break;
        case Keys::EffectRoute::Pane:
            // The pane is the ui task's render state: ask for it.
            App::Ui::requestScene((uint8_t)MegaOpen::MediaPane::sceneFromName(m.arg));
            break;
#else
        case Keys::EffectRoute::Panel:
        case Keys::EffectRoute::Pane:
            LogBus::line("[macro] this build has no panel - see FEATURE_DISPLAY_CAN");
            break;
#endif
        default:
            break;
    }
}

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
    if (d != affa::Direction::Tx) g_panelWatch.onRx(f.id, f.len, f.data, millis());
    // Feed the raw frame ring: the panel bus is not a car source.
    ringPut((d == affa::Direction::Tx) ? 'T' : 'R', f.id, f.data, f.len);

    // Feed the per-ID table too: it keeps a single sighting (e.g. the panel's
    // `1C1 70`) that the 24-entry ring loses within ~15 ms in a storm.
    // Received frames only. NOT when a car task runs: the table is then its
    // own (one writer), and holds the car's bus, not the panel's.
#if FEATURE_VEHICLE_CAN
    if (d != affa::Direction::Tx && !App::Car::running())
        g_sniffer.ingest(f.id, f.data, f.len > 8 ? 8 : f.len, millis());
#endif

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
        logf("[affa] ... %u frames lost\n",
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

// The head unit's clock: undocumented `0x9D` = `[hours][minutes][00]`, once a
// minute, unprompted (verified against wall time; docs/raise-protocol.md §3 says
// there is none). Range-checked, since other units may use 0x9D differently.
// On the hu task: the clock and the media header are App::Ui's, so this only
// asks (postHuClock), and the ui task posts it and retries until delivered.
static void onHuClock(const uint8_t* payload, uint8_t len)
{
    if (len < 2 || payload[0] > 23 || payload[1] > 59) return;
    // Once a minute is the unit's own pace; logging each is cheap.
    static uint16_t s_last = 0xFFFF;
    const uint16_t hm = (uint16_t)((payload[0] << 8) | payload[1]);
    if (hm == s_last) return;                        // same minute, nothing to redraw
    s_last = hm;
#if FEATURE_DISPLAY_CAN
    App::Ui::postHuClock(hm);
#endif
    LogBus::printf("[hu] clock from head unit: %02u:%02u",
                   (unsigned)payload[0], (unsigned)payload[1]);
}

// Every head-unit frame, on the hu task, after its ack went out.
static void onHuFrame(uint8_t cmd, const uint8_t* payload, uint8_t len, void*)
{
    g_huDialect.onFrame(cmd);
    if (cmd == 0x9D) onHuClock(payload, len);
    // Static: the hu task is the only caller, and its stack is 3 kB. Web only
    // (the [hur] tag): the [hu] echo already logs every frame once.
    static char b[LogBus::MAX_LINE];
    const uint16_t total = g_huRx.lastTotalLen();
    int n = snprintf(b, sizeof(b), "%sR cmd=%02X len=%u", HUR, cmd, (unsigned)total);
    if (total > len) n += snprintf(b + n, sizeof(b) - n, " (+%u trunc)", (unsigned)(total - len));

    for (uint8_t i = 0; i < len && n < (int)sizeof(b) - 8; ++i)
        n += snprintf(b + n, sizeof(b) - n, " %02X", payload[i]);

    uint8_t printable = 0;
    for (uint8_t i = 0; i < len; ++i)
        if (payload[i] >= 0x20 && payload[i] < 0x7F) ++printable;

    // Text if at least half the payload is printable.
    const bool looksLikeText = (len >= 4 && printable * 2 >= len);
    if (looksLikeText && n < (int)sizeof(b) - (int)len - 8) {
        n += snprintf(b + n, sizeof(b) - n, "  |");
        for (uint8_t i = 0; i < len && n < (int)sizeof(b) - 4; ++i) {
            const uint8_t c = payload[i];
            b[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        b[n++] = '|';
        b[n] = '\0';
    }
    LogBus::line(b);
    huRecord('R', cmd, payload, len, total);

#if FEATURE_DISPLAY_CAN || FEATURE_BLE
    // Every frame: it is also the head unit's liveness. HuMediaSource decides
    // what is text; `looksLikeText` is only for the log column.
    g_hu.onFrame(cmd, payload, len, millis());
#endif

    // 0x90: the head unit requests a message by command byte in payload[0]
    // (spec len-2 form and DUDU len-1 form alike). The answer goes in the
    // reply lane, ahead of queued telemetry.
    if (cmd == 0x90 && len >= 1) {
        const bool sent = App::Hu::sendByCmd(payload[0]);
        LogBus::printf("[hu] 0x90: head unit asked for %02X -> %s", payload[0],
                       sent ? "sent" : "nothing to say");
    }
}

// Bytes outside any frame, batched so they log as lines, not single bytes.

static uint8_t  g_junk[32];
static uint8_t  g_junkLen = 0;
static uint32_t g_junkAtMs = 0;

// The hu task. Junk goes in the ring (its shape identifies a foreign dialect),
// and the ring's echo shows it on Serial and the web.
static void onHuJunk(uint8_t b, void*)
{
    g_huDialect.onJunk(b);
    g_junk[g_junkLen++] = b;
    g_junkAtMs = millis();
    if (g_junkLen >= sizeof(g_junk)) {
        huRecord('J', 0, g_junk, g_junkLen, g_junkLen);
        g_junkLen = 0;
    }
}

// The hu task's per-pass housekeeping (HuTask Setup::wake): a junk run that
// has gone quiet is flushed as one entry, then the ring's echo catches up.
static void huWake(uint32_t now)
{
    if (g_junkLen && (uint32_t)(now - g_junkAtMs) > 50) {
        huRecord('J', 0, g_junk, g_junkLen, g_junkLen);
        g_junkLen = 0;
    }
#if FEATURE_DISPLAY_CAN || FEATURE_BLE
    // Text frames a busy media mutex held back (HuMediaSource::onFrame).
    g_hu.service();
#endif
    huDrainSerial();
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
    d[Keys::DEST_UART] = {r.uartCls, r.uartOn && g_hu.heard(millis()), false};
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
                LogBus::printf("[res] learn %s: no rung now - the map changed",
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
    h.route.macroRun = [](uint8_t idx, void*) { g_macros.run(idx); };
    App::KeysTask::begin(h, &g_tx);
}

// ---- net_svc's hooks (src/app/Housekeeping.h) ---------------------------------
// WiFi usable at least once since boot: the next OTA needs it. Our own AP
// counts: the next OTA arrives through it just as well. Counting STA only
// meant an image flashed in the car (AP, no known network) was rolled back
// and rebooted 180 s after every OTA.
static bool hkWifiSeen()
{
    static bool s_seen = false;
    s_seen = s_seen || WiFi.status() == WL_CONNECTED ||
             (!g_wifi.connecting() && !g_wifi.isSta() && (WiFi.getMode() & WIFI_AP));
    return s_seen;
}

// WiFi, then the board's address on the panel once per connection
// (src/net/IpNotice.h): "IP 192.168.1.15 / megaopen.local" on the home
// network, "AP MegaOpen-XXXX / 192.168.4.1" on our own. Polite: it waits out
// the menu and a glass hold (App::Ui::queueNotice).
static void hkWifi(uint32_t now)
{
    g_wifi.loop(now);
#if FEATURE_DISPLAY_CAN
    static IpNotice s_notice;
    IpNotice::Mode m = IpNotice::Mode::None;
    uint32_t ip = 0;
    if (g_wifi.connecting()) {
        // a join or a scan: neither link is settled
    } else if (g_wifi.isSta()) {
        if (WiFi.status() == WL_CONNECTED) { m = IpNotice::Mode::Sta; ip = (uint32_t)WiFi.localIP(); }
    } else if (WiFi.getMode() & WIFI_AP) {
        m = IpNotice::Mode::Ap;
        ip = (uint32_t)WiFi.softAPIP();
    }
    if (!s_notice.tick(now, m, ip)) return;
    char head[40];
    String body;
    if (m == IpNotice::Mode::Sta) {
        snprintf(head, sizeof(head), "IP %s", WiFi.localIP().toString().c_str());
        body = g_wifi.mdnsHost();          // empty with mDNS off
    } else {
        snprintf(head, sizeof(head), "AP %s", g_wifi.apSsid().c_str());
        body = WiFi.softAPIP().toString();
    }
    App::Ui::queueNotice(head, body.c_str());
    logf("[wifi] on the panel: %s / %s\n", head, body.c_str());
#endif
}

// Every 10 s, after [win] and [task]: the lines nobody else owns.
static void hkBeat(uint32_t now)
{
    const uint32_t s = now / 1000;
    // [stk] is a subset of [task] now. Kept until Step 6 retires it, so old
    // logs and new ones still compare line for line.
    const TaskHandle_t net = App::Housekeeping::task();
    logf("[stk] loop=%u net=%u ble=%u knobs=%u",
         g_loopTask ? (unsigned)uxTaskGetStackHighWaterMark(g_loopTask) : 0u,
         net ? (unsigned)uxTaskGetStackHighWaterMark(net) : 0u,
         g_bleTask ? (unsigned)uxTaskGetStackHighWaterMark(g_bleTask) : 0u,
#if FEATURE_KNOBS
         (unsigned)g_knobs.stackFree());
#else
         0u);
#endif
    // [hu] and [key] are logged by the hu and keys tasks themselves, [car]
    // by the car task, and [can], [panel], [stall] and [loop] by the loop,
    // each from its own window.
#if FEATURE_CAN2
    // CAN A, from the car task's snapshot: no SPI from here. A silent car bus,
    // a deaf chip and a wrong rate look the same without these.
    // ab = searching | locked | manual at bps; fps = the last second.
    {
        const Mcp2518Source::Status c = Mcp2518Source::instance().status().read();
        static const char* const kAb[] = {"searching", "locked", "manual"};
        logf("[can2] live=%u ab=%s bps=%lu fps=%lu frames=%lu hwOvf=%lu ringPeak=%lu/256 "
             "fd=%lu rtr=%lu intKicks=%lu maskMaxUs=%lu rec=%lu tec=%lu flags=0x%02lX "
             "bdiag1=0x%08lX restarts=%lu rounds=%lu rejected=%lu err=0x%lX",
             (unsigned)c.live, kAb[c.state <= 2 ? c.state : 0], (unsigned long)c.bps,
             (unsigned long)c.fps, (unsigned long)c.frames, (unsigned long)c.hwOverflows,
             (unsigned long)c.ringPeak, (unsigned long)c.fdDropped,
             (unsigned long)c.remoteDropped, (unsigned long)c.intKicks,
             (unsigned long)c.maskMaxUs, (unsigned long)(c.trec & 0xFF),
             (unsigned long)((c.trec >> 8) & 0xFF), (unsigned long)((c.trec >> 16) & 0x3F),
             (unsigned long)c.bdiag1, (unsigned long)c.restarts, (unsigned long)c.cycles,
             (unsigned long)c.rejected, (unsigned long)c.beginErr);
        // A newly detected rate goes to NVS here, on net_svc, once: the car
        // task never waits for a flash write, and the next boot tries it first.
        if (c.detected && c.detected != g_cfg.s().can2Detected) {
            g_cfg.saveCan2Detected(c.detected);
            logf("[can2] %lu saved: tried first at the next boot", (unsigned long)c.detected);
        }
    }
#endif
#if FEATURE_BLE
    // The BLE key path since boot: q = the keys-to-ble_svc queue's drops;
    // ams = AMS writes that went out, fail = ones the stack refused;
    // stale = waited over 300 ms; coal = volume presses folded into one
    // pending count; nolink = owed with no phone connected; vol = AMS
    // commands a volume press sends (measured, sinks/AmsVolumeStep.h).
    logf("[ble] keys q=%u ams=%u fail=%u hid=%u stale=%u coal=%u nolink=%u vol=%u",
         (unsigned)Sinks::BleKeyQueue::dropped(), (unsigned)g_bleKeys.amsWrites(),
         (unsigned)g_bleKeys.amsFailed(),
         (unsigned)g_bleKeys.hidPresses(), (unsigned)g_bleKeys.stale(),
         (unsigned)g_bleKeys.coalesced(), (unsigned)g_bleKeys.noLink(),
         (unsigned)g_bleKeys.volSteps());
#endif
#if FEATURE_DISPLAY_CAN || FEATURE_BLE
    // lockMiss = head-unit frames that found a reader holding the media
    // mutex and were applied a pass later; lost = of those, stash overflow.
    logf("[media] hu frames=%u text=%u lockMiss=%u lost=%u",
         (unsigned)g_hu.frames(), (unsigned)g_hu.textFrames(),
         (unsigned)g_hu.lockMisses(), (unsigned)g_hu.stashDropped());
#endif
    logf("[hb] up=%02u:%02u:%02u heap=%u min=%u big=%u canrx=%u keys=%u tx=%u/%u",
         (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60),
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
         (unsigned)ESP.getMaxAllocHeap(),
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
         (unsigned)g_canRx,
#else
         0u,
#endif
         (unsigned)g_engine.events(),
         (unsigned)g_tx.keyFrames(), (unsigned)g_tx.vehicleFrames());
}

// Once a minute: address, SSID, BSSID and gateway, to find the board and
// confirm which network (and radio) it is really on. Not every 10 s: WiFi.SSID()
// and the toString() calls allocate, and this was the loop's String work.
static void hkBeatSlow(uint32_t)
{
    if (g_wifi.connecting()) {
        logf("[net] joining \"%s\"", g_wifi.curSsid().c_str());
    } else if (g_wifi.isSta()) {
        // A String whose allocation failed has a NULL buffer; strlen(NULL)
        // in vsnprintf reboots. Print "?" instead.
        auto nn = [](const char* s) { return s ? s : "?"; };
        logf("[net] STA ssid=\"%s\" rssi=%d bssid=%s ip=%s gw=%s mask=%s %s",
             nn(WiFi.SSID().c_str()), (int)WiFi.RSSI(), nn(WiFi.BSSIDstr().c_str()),
             nn(WiFi.localIP().toString().c_str()),
             nn(WiFi.gatewayIP().toString().c_str()),
             nn(WiFi.subnetMask().toString().c_str()),
             g_wifi.mdnsHost().length() ? nn(g_wifi.mdnsHost().c_str()) : "(no mdns)");
    } else {
        logf("[net] AP \"%s\" ip=%s %s", g_wifi.apSsid().c_str(),
             g_wifi.ip().c_str(),
             g_wifi.mdnsHost().length() ? g_wifi.mdnsHost().c_str() : "(no mdns)");
    }
}

// Why this boot happened, and how many there have been: tells a real reboot
// (panic, watchdog, brownout) from a freeze that only looked like one.
static void logBootReason()
{
    const esp_reset_reason_t r = esp_reset_reason();
    // One token each, so the [boot] line splits on spaces (and a grep for
    // reason=task_wdt finds a watchdog reboot).
    const char* why;
    switch (r) {
        case ESP_RST_POWERON:   why = "poweron";   break;
        case ESP_RST_EXT:       why = "ext";       break;   // the EN/reset pin
        case ESP_RST_SW:        why = "sw";        break;   // esp_restart(), OTA
        case ESP_RST_PANIC:     why = "panic";     break;   // a crash
        case ESP_RST_INT_WDT:   why = "int_wdt";   break;
        case ESP_RST_TASK_WDT:  why = "task_wdt";  break;
        case ESP_RST_WDT:       why = "wdt";       break;   // any other watchdog
        case ESP_RST_DEEPSLEEP: why = "deepsleep"; break;
        case ESP_RST_BROWNOUT:  why = "brownout";  break;   // the supply sagged
        case ESP_RST_SDIO:      why = "sdio";      break;
        default:                why = "unknown";   break;
    }
    Preferences p;
    p.begin("sys", false);
    const uint32_t boots = p.getUInt("boots", 0) + 1;
    p.putUInt("boots", boots);
    // Abnormal resets counted separately: a number that should stay at zero.
    const bool bad = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                     r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
    uint32_t crashes = p.getUInt("crashes", 0);
    if (bad) p.putUInt("crashes", ++crashes);
    p.end();
    // key=value, one line, the build first: the flash log keeps a file per
    // boot, and this is the line that says which image wrote it and why the
    // board came up. psram=0 on a board that should have it is a wrong image.
    logf("[boot] build=%s fw=" MEGAOPEN_VERSION " board=%s reason=%s boots=%u crashes=%u psram=%u",
         MegaOpen::buildId(), Board::NAME, why, (unsigned)boots, (unsigned)crashes,
         (unsigned)ESP.getPsramSize());
}

void setup()
{
    // The log ring into PSRAM before anything logs (LogBus::grow()).
    const bool ringGrown = LogBus::grow();
    Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    // USB-CDC console (T-2CAN): with no host reading, a write waits out the
    // timeout. In the car nothing reads it; the LogBus task must not stall on it.
    Serial.setTxTimeoutMs(0);
#endif
    delay(200);

    // Logging first: every line from here on goes through the LogBus.
    LogBus::addSink(serialSink);
#if FEATURE_LOGFS
    const bool logfs = LogFile::begin();
    if (logfs) {
        LogBus::addSink(fileSink);
        // Run on the LogBus task by flushAndWait(): the flash log's RAM buffer
        // to /log/0.log, synced.
        LogBus::onFlush(LogFile::flushNow);
    }
#endif
    // Below the loop task (raised in setup()'s tail), on the core WiFi lives on.
    LogBus::startTask(App::Plan::LOGBUS_PRIO, App::core(App::Plan::LOGBUS_CORE));
#if FEATURE_CAPTURE
    // The CAN recorder, after the flash log has mounted littlefs (they share
    // it). The car task pushes into it from its first frame.
    Capture::begin();
#endif
#if FEATURE_LOGFS
    if (logfs) esp_register_shutdown_handler(logShutdown);
#endif
    // The registry, from its first entries. This task is the loop (setup()
    // runs on it) and it is critical: it owns the panel and the CAN driver,
    // and net_svc's OTA verdict reads its age. The LogBus is not: a stuck log
    // costs evidence, not keys.
    g_loopTaskId = App::Tasks::add("loop", true);
    g_loopTask = xTaskGetCurrentTaskHandle();
    App::Tasks::setHandle(g_loopTaskId, g_loopTask);
    g_logbusTaskId = App::Tasks::add("logbus", false);
    App::Tasks::setHandle(g_logbusTaskId, LogBus::task());
    LogBus::onWake([](uint32_t busyUs) { App::Tasks::beat(g_logbusTaskId, busyUs); });
    logBootReason();
    logf("[log] ring %d B in %s%s", LogBus::ringSize(), LogBus::ringInPsram() ? "PSRAM" : "internal RAM",
         ringGrown ? "" : " (the boot ring: no memory for a bigger one)");
#if FEATURE_LOGFS
    if (logfs)
        logf("[log] flash log /log/0.log (previous boot: /log/1.log), %u/%u kB used; "
             "written every %u kB, on warnings and before a reboot",
             (unsigned)(LogFile::usedBytes() / 1024), (unsigned)(LogFile::totalBytes() / 1024),
             (unsigned)(LogFile::FLUSH_BYTES / 1024));
    else
        logf("[log] flash log FAILED: %s; RAM and serial only", LogFile::mountError());
#endif

    memMark("boot");
    // Tap the system log first, so boot output reaches the browser too.
    s_idfVprintf = esp_log_set_vprintf(logTapVprintf);
    Log::setLevel(LogLevel::INF);

    g_cfg.begin();
    g_cfg.loadKeyMap(g_map);
#if FEATURE_DISPLAY_CAN
    // The render policy's settings from NVS, what it draws from, and its UiQ
    // (before the panel can post a verdict to it). The task starts below,
    // when there is a panel.
    {
        App::Ui::Deps d;
        d.ops    = &g_panelOps;
        d.router = &g_media;
        d.hu     = &g_hu;
        d.engine = &g_engine;
        d.car    = &g_state;
#if FEATURE_BLE
        d.amsActive      = []() { return BleHub::amsActive(); };
        d.phoneConnected = []() { return Bluetooth::IsConnected(); };
        d.phoneTimeSet   = []() { return Bluetooth::IsTimeSet(); };
#endif
        App::Ui::begin(g_cfg, d);
    }
    // The panel menu: reads the settings and the car, hands its changes to
    // net_svc (hkWeb), which stores them like any other setting.
    {
        App::PanelMenu::Deps m;
        m.ops = &g_panelOps;
        m.cfg = &g_cfg;
        m.car = &g_state;
#if FEATURE_KNOBS
        // Get and set together under the settings lock (applyPending holds it),
        // like every knob read-modify-write (Knobs::Service's comment).
        m.knobsGet = [](Knobs::Config& out) { out = g_knobs.config(); return true; };
        m.knobsSet = [](const Knobs::Config& c) { g_knobs.setConfig(c); g_cfg.saveKnobs(c); };
#endif
        m.stageUi = [](const Cfg::Settings& s) {
            if (!App::Ui::stageFrom(s)) logf("[menu] ui settings busy: shown after the next change");
        };
        m.stageMedia = [](const Cfg::Settings& s) {
            g_media.setMode(MediaRouter::modeFromStr(s.mediaSource.c_str()));
        };
        App::PanelMenu::begin(m);
    }
#endif
#if FEATURE_DISPLAY_CAN
    g_cfg.loadAffaMap(g_affaMap);
    g_affaCfg.init(g_affaMap);
#endif
    {
        App::KeysTask::KeyMapCfg k;
        k.map         = g_map;
        k.encInverted = g_cfg.s().encInverted;
        k.timing      = g_cfg.s().timing;
        g_keymapCfg.init(k);
    }

#if FEATURE_MATRIX
    for (uint8_t c = 0; c < Board::MATRIX_COLS; ++c) pinMode(Board::PIN_COL[c], INPUT_PULLUP);
    for (uint8_t r = 0; r < Board::MATRIX_ROWS; ++r) pinMode(Board::PIN_ROW[r], INPUT);
#endif

    // The hu task drains up to 512 B a pass; the 256 B default ring would
    // overflow first. Only takes effect before begin().
    Serial1.setRxBufferSize(1024);
    Serial1.begin(g_cfg.s().uartBaud, SERIAL_8N1, Board::PIN_HU_RX, Board::PIN_HU_TX);
    g_tx.begin(&Serial1);
    g_tx.onTap(uartTap, nullptr);

    g_scanner.begin(driveRow, readCols, settle, nullptr);
    g_keySink.begin(writeKeyFrame, nullptr);
    // The release rule needs "now": a detent that waited on the KeyBus still
    // gets MIN_GAP_MS before its release (keys/IKeySink.h).
    g_keySink.setClock([]() -> uint32_t { return millis(); });
    g_keySink.setEnabled(g_cfg.s().uartEnabled);
    g_keySink.setClasses(g_cfg.s().routeUart);
    g_keySink.setPulseMs(g_cfg.s().pulseMs);
    {
        App::KeysTask::Routing r;
        r.uartOn  = g_cfg.s().uartEnabled;
        r.uartCls = g_cfg.s().routeUart;
        r.pulseMs = g_cfg.s().pulseMs;
        r.resOn   = g_cfg.s().resEnabled;
        r.resCls  = g_cfg.s().routeRes;
        r.amsCls  = g_cfg.s().routeAms;
        r.hidCls  = g_cfg.s().routeHid;
        r.usbCls  = g_cfg.s().routeUsb;
        g_routingCfg.init(r);
    }

    // The head-unit link: the emitter is the hu task's from here on. Tuning
    // from NVS (every ignition is a reboot); the emitter sends only when the
    // owner has switched it on (remembered) and the UART is on.
    {
        App::Hu::EmitterCfg ec;
        g_cfg.loadTuning(ec.tuning);
        ec.enabled = g_cfg.loadHuSend() && g_cfg.s().uartEnabled;
        ec.exp     = g_cfg.loadHuExp();
        App::Hu::Setup hs;
        hs.port    = &Serial1;
        hs.tx      = &g_tx;
        hs.rx      = &g_huRx;
        hs.emitter = &g_emitter;
        hs.state   = &g_state;
        hs.wake    = huWake;
        App::Hu::begin(hs, ec);
    }

    g_engine.begin(&g_scanner, &g_map);
    g_engine.setTiming(g_cfg.s().timing);
    g_engine.setEncoderInverted(g_cfg.s().encInverted);
    g_engine.addSink(&g_keySink);
#if FEATURE_USB_HID
    g_usbKeys.begin();
    g_usbKeys.setClasses(g_cfg.s().routeUsb);
    g_engine.addSink(&g_usbKeys);
    logf("[usb] HID consumer control on the native USB, classes=0x%02X",
         (unsigned)g_usbKeys.classes());
#endif
#if FEATURE_RESISTIVE_KEYS
    // Load the ladder first: begin() opens both channels, clearing whatever
    // state a reset left the pins in (else the head unit sees a held button).
    g_cfg.loadResMap(g_resMap);
    g_resMapCfg.init(g_resMap);
    g_resSink.begin(resApply, nullptr, &g_resMap);
    g_resSink.setEnabled(g_cfg.s().resEnabled);
    g_resSink.setClasses(g_cfg.s().routeRes);
    g_engine.addSink(&g_resSink);
    {
        // Printed every boot: legs and rung values live in NVS.
        char b[96];
        int n = snprintf(b, sizeof(b), "[res] ch0 legs=%u ch1 legs=%u drive=%s%s ohms:",
                         (unsigned)Board::RES_LEGS_CH0, (unsigned)Board::RES_LEGS_CH1,
                         g_resMap.activeHigh() ? "mosfet" : "gpio",
                         g_cfg.s().resEnabled ? "" : " (DISABLED)");
        for (uint8_t i = 0; i < Board::RES_LEGS_CH0 && n < (int)sizeof(b) - 8; ++i)
            n += snprintf(b + n, sizeof(b) - n, " %u", (unsigned)g_resMap.ladder(0).ohmsFor((uint8_t)(1u << i)));
        logf("%s", b);
    }
#endif
    g_engine.onEvent(onKeyEvent, nullptr);
#if FEATURE_MATRIX
    // The scan task posts; only the keys task dispatches.
    g_engine.onEmit(matrixEmit, nullptr);
#endif

    // The macro filter sees every event before any sink; only it may stop one.
    g_cfg.loadMacros(g_macros);
    {
        Keys::MacroTable t;
        g_macros.table(t);
        g_macroCfg.init(t);
    }
    g_macros.onFire(macroFire, nullptr);
    g_macros.onEmit(macroEmit, nullptr);
    g_engine.onFilter(macroFilter, nullptr);
    {
        uint8_t on = 0;
        for (uint8_t i = 0; i < Keys::MacroEngine::count(); ++i)
            if (g_macros.at(i).enabled && g_macros.at(i).valid()) ++on;
        logf("[macro] %u of %u active", (unsigned)on, (unsigned)Keys::MacroEngine::count());
    }

#if FEATURE_KNOBS
    // After the macro filter: the knob task starts sampling inside begin().
    {
        Knobs::Config kc;
        g_cfg.loadKnobs(kc);
        // Each event straight onto the KeyBus: a click as one Tap item, so a
        // full lane drops the press and its release together or not at all.
        g_knobs.begin(kc, [](const Keys::Event& e, bool pair, void*) {
            return pair ? App::KeyBus::postTap(e) : App::KeyBus::postEvent(e);
        }, nullptr);
        memMark("knobs");
        // Printed every boot: pins and enable state.
        logf("[knobs] S1=GPIO%d S2=GPIO%d KEY=GPIO%d pot=GPIO%d led=GPIO%d  enc=%s pot=%s led=%s x%u+%u feet cap=%umA",
             (int)Board::PIN_KNOB_S1, (int)Board::PIN_KNOB_S2, (int)Board::PIN_KNOB_KEY,
             (int)Board::PIN_KNOB_POT, (int)Board::PIN_KNOB_LED,
             kc.enc[Knobs::ENC_RIGHT].enabled ? "on" : "OFF", Knobs::potModeName(kc.potMode),
             kc.ledEnabled ? "on" : "OFF",
             (unsigned)(kc.zone[0].count + kc.zone[1].count),
             (unsigned)kc.feetCount, (unsigned)kc.maxMilliamps);
    }
#endif

    // The ack is the hu task's own (App::Hu::begin): written the moment a
    // frame ends, before these run.
    g_huRx.onFrame(onHuFrame, nullptr);
    g_huRx.onJunk(onHuJunk, nullptr);

#if FEATURE_DISPLAY_CAN || FEATURE_BLE
    // The head unit as a media source, always registered; `mediaSource` selects.
    // Command bytes from NVS; 0 = learn which carries the title (HuMediaSource).
    g_hu.setChannels(g_cfg.s().huTitleCmd, g_cfg.s().huArtistCmd);
    g_hu.setTitleFirst(g_cfg.s().huTitleFirst);
    g_hu.setChangeCallback(onMediaChange, nullptr);
    // The AMS slot is filled by the BLE block below, if it starts.
    g_media.setSources(nullptr, &g_hu);
    {
        MediaRouter::Mode m = MediaRouter::modeFromStr(g_cfg.s().mediaSource.c_str());
#if !FEATURE_BLE
        // No radio in this image, and `ams` is the stored default: force `hu`.
        if (m == MediaRouter::Mode::Ams) {
            LogBus::line("media source: NVS says 'ams', but this build has no radio "
                         "— forcing 'hu'");
            m = MediaRouter::Mode::Hu;
        }
#endif
        g_media.setMode(m);
    }
#endif

#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
    logf("[can] rx=GPIO%d tx=GPIO%d", (int)Board::PIN_CAN_RX, (int)Board::PIN_CAN_TX);
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
            logf("[can] vehicle: TwaiLink::begin FAILED — no controller (it keeps trying)");
    }
    logf("[can] vehicle: listen-only, never transmits, never ACKs, "
         "cannot go bus-off. No display on this bus.");
    logf("[can] no termination resistor needed here — an OBD tap is not a "
         "two-node stub");
#endif

#if FEATURE_DISPLAY_CAN
    // Through the LogBus like every other line: Serial, the flash log and the
    // web log. Runs inside poll(): LogBus::line only
    // copies into its ring.
    g_affaLog.begin([](const char* line) { LogBus::line(line); });

    const String& dt = g_cfg.s().displayType;
    {
        // `carminat`: the same object typed as CarminatDisplay (styled text,
        // message boxes, the 0x1F1 pane); null on an UpdateList panel. No RTTI
        // here. From here on the object is App::Panel's (setup() runs on the
        // loop task, which is the panel task) and PanelOps' for rendering.
        affa::CarminatDisplay* carminat = nullptr;
        affa::AffaDisplayBase* display =
            MegaOpen::createDisplay(dt.c_str(), g_twai, g_affaClock, &carminat);
        g_panelOps.attach(display, carminat);
        // Callbacks before begin(), or the first poll()'s sync transition is
        // missed: App::Panel counts the verdicts, and every ticket's verdict
        // goes on to the ui task's UiQ.
        App::Panel::Setup ps;
        ps.display  = display;
        ps.carminat = carminat;
        ps.link     = &g_twai;
        ps.ops      = &g_panelOps;
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
        logf(ok ? "[can] display: TwaiLink @500k normal, rxq=%u"
                : "[can] display: TwaiLink::begin FAILED — no controller (it keeps trying)",
             (unsigned)cfg.rxQueue);
    }
    memMark("can+display");
    // Listen before talking, when we register: the TX gate stays shut until
    // PanelBusWatch has listened for LISTEN_MS from the first poll() with no
    // other master's sync (panelWatchStep()), so an OEM radio is found before
    // we send one. With FuncsReg off we draw over the radio: no gate.
    g_twai.setTxEnabled(!g_cfg.s().funcReg);
    g_web.attachPanelWatch(&g_panelWatch);
    // Passive = a real radio owns the handshake; we send no sync or hello and
    // never register. Must be set if an OEM radio is on the bus.
    panel.setPassive(!g_cfg.s().funcReg);

    // The default, stated: the library powers the panel on during the opening.
    panel.setAutoPower(true);

    // Keys arrive through onPanelKey() with their decode time; the clock is
    // for AffaKeySource::handle() only. See AffaKeySource::NowFn.
    g_affaKeys.begin(&g_engine, &g_affaMap, []() -> uint32_t { return millis(); });
    g_affaKeys.onReport(&onAffaKeyReport, nullptr);
    // onPanelKey only posts to the KeyBus; the keys task maps and dispatches.
    panel.onKey(&onPanelKey, nullptr);

    // Every AFFA frame to USB serial, gated on log level TRC.
    g_twai.onRaw(affaTap, nullptr);
    logf("[affa] frame trace follows the log level - raise it to TRC (logLevel: the Service page or the panel menu); it will slow the loop");

    // begin() transmits nothing; the first heartbeat leaves on the first poll().
    panel.begin();
    memMark("display.begin");

    logf("[MegaOpen] display=%s funcreg=%s\n",
                  dt.c_str(), g_cfg.s().funcReg ? "on" : "off");
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

#if FEATURE_BLE
    // Before any key can be posted, BLE on or not: a proxy with no ble_svc
    // behind it drops and counts.
    if (!Sinks::BleKeyQueue::begin())
        logf("[ble] KEY QUEUE FAILED TO ALLOCATE - phone and BLE keyboard keys are dead");
    {
        const String& bm = g_cfg.s().bleMode;
        // Off = BleHub not started (saves ~40 KB of host state).
        const bool bleOn = (bm == "ams" || bm == "keyboard" || bm == "both");
        // For /api/health. Not BleHub::amsActive() alone: its gMode defaults to
        // Ams even when BLE never started.
        g_amsSinkLive = bleOn && BleHub::amsActive(bm == "keyboard" ? BleHub::Mode::Keyboard
                                                  : bm == "both"   ? BleHub::Mode::Both
                                                                   : BleHub::Mode::Ams);
        g_hidSinkLive = bleOn && BleHub::hidActive(bm == "keyboard" ? BleHub::Mode::Keyboard
                                                  : bm == "both"   ? BleHub::Mode::Both
                                                                   : BleHub::Mode::Ams);
        BleHub::Mode mode = BleHub::Mode::Ams;
        if      (bm == "keyboard") mode = BleHub::Mode::Keyboard;
        else if (bm == "both")     mode = BleHub::Mode::Both;

        if (bleOn) {
            if (BleHub::amsActive(mode)) {
                g_ams.begin();                     // AMS callback -> MediaInfo
                g_ams.setChangeCallback(onMediaChange, nullptr);
                // Pass both: a null here would unregister the head-unit source.

                g_media.setSources(&g_ams, &g_hu);
                g_engine.addSink(&g_amsKeys);
            }
            if (BleHub::hidActive(mode)) g_engine.addSink(&g_hidKeys);

#if FEATURE_ANCS
            // ANCS rides the AMS connection.
            if (BleHub::amsActive(mode))
                AppleNotificationService::RegisterForNotifications(onAncsNotification);
#endif

            // Key classes per destination come from config, editable at runtime
            // (staged in g_routingCfg, applied by the keys task).
            g_amsKeys.setClasses(g_cfg.s().routeAms);
            g_hidKeys.setClasses(g_cfg.s().routeHid);

            // Begin() builds the GATT tree and needs more stack than setup() has.
            static BleHub::Mode s_mode = mode;
            xTaskCreate([](void*) {
                            // Short name "MEGA" in the advertising packet, full
                            // name in the scan response: "MegaOpen" filled the
                            // 31-byte packet exactly and iPhones never paired
                            // (MeganeCAN's "MCD1" leaves 4 bytes spare).
                            BleHub::Begin(s_mode, "MEGA", "MegaOpen");
                            memMark("ble.begin");   // on this task, so it interleaves
                            // Only after the GATT tree exists. Not critical:
                            // GATT discovery blocks for seconds, and a BLE
                            // fault must never cost the keys an OTA rollback.
                            App::TaskSpec spec;
                            spec.name  = "ble_svc";
                            spec.fn    = bleServiceTask;
                            spec.stack = App::Plan::BLE_STACK;
                            spec.prio  = App::Plan::BLE_PRIO;
                            spec.core  = App::Plan::BLE_CORE;
                            spec.idOut = &g_bleTaskId;
                            g_bleTask = App::spawn(spec);
                            vTaskDelete(nullptr);
                        },
                        "bt_begin", 16384, nullptr, 1, nullptr);
            logf("[MegaOpen] BLE mode=%s\n", bm.c_str());
        }

        // Warn when the media screen is on but no AMS role will feed it.
#if FEATURE_DISPLAY_CAN
        const bool mediaAuto = App::Ui::mediaAuto();
#else
        const bool mediaAuto = false;
#endif
        if (mediaAuto && !(bleOn && BleHub::amsActive(mode)))
            logf("[MegaOpen] media screen is ON but bleMode=%s has no AMS "
                          "role — rows will stay empty until it is 'ams' or 'both'\n",
                          bm.c_str());
    }
#endif

    // The two core-1 tasks that write the head-unit UART. Here, with every
    // key sink registered and the parser's callbacks set: the panel cannot
    // deliver a key before loop() polls it, and the knobs' early posts wait
    // on the KeyBus for the keys task's first pass.
    keysStart();
    App::Hu::start();
    memMark("keys+hu");
#if FEATURE_DISPLAY_CAN
    // The render policy on its own task (priority 2, core 1): the panel task
    // only pumps now.
    if (App::Panel::present()) {
        App::Ui::start();
        memMark("ui");
    }
#endif

    g_wifi.begin(g_cfg);
    memMark("wifi");
    g_web.attachWifi(&g_wifi);
    g_web.attachCounters(&g_ghosts, &g_encSteps);
    g_web.attachMacros(&g_macros);
    {
        // What the web edits and another task applies (see g_routingCfg).
        WebUi::Staging st;
        st.routing = &g_routingCfg;
#if FEATURE_BLE
        st.volToAms = []() { return g_volToAms.load(); };
#endif
        st.keymap  = &g_keymapCfg;
        st.macros  = &g_macroCfg;
#if FEATURE_DISPLAY_CAN
        st.affa    = &g_affaCfg;
#endif
#if FEATURE_RESISTIVE_KEYS
        st.resAllOff = []() { g_resAllOffReq.post(1); App::KeyBus::kick(); };
        st.resMap    = &g_resMapCfg;
        st.resLearn  = [](const App::KeysTask::ResLearn& q) {
            g_resLearnReq.post(q);
            App::KeyBus::kick();
        };
#endif
        g_web.attachStaging(st);
    }
#if FEATURE_RESISTIVE_KEYS
    g_web.attachResistive(&g_resSink);
#endif
#if FEATURE_KNOBS
    g_web.attachKnobs(&g_knobs);
#endif
#if FEATURE_CAN2
    // CAN A: the car's bus on the board itself. Listen-only in the controller, its rate found by listening; started
    // on the car task, which owns the SPI from its first instruction.
    Mcp2518Source::instance().configure(g_cfg.s().can2Bitrate, g_cfg.s().can2Detected);
    g_web.attachCan2(&Mcp2518Source::instance().status(),
                     [](uint32_t bps) { Mcp2518Source::instance().requestRate(bps); });
#endif
    // ---- the car task: its sources are chosen here, at boot -----------------
    // CAN A on the T-2CAN, the TWAI on env:direct. None:
    // no task, and the loop does its chores (App::Car::service).
    {
        App::Car::Setup cs;
#if FEATURE_CAN2
        cs.src[cs.n++] = &Mcp2518Source::instance();
#endif
#if FEATURE_VEHICLE_CAN
#if !FEATURE_DISPLAY_CAN
        cs.src[cs.n++] = &TwaiVehicleSource::instance();
#endif
        cs.sniffer = &g_sniffer;
#endif
        cs.state = &g_state;
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
        cs.tap = carTap;
#endif
        // The user's decode map from NVS (none: the factory table), read into
        // a buffer that lives for begin() only.
        uint8_t* blob = (uint8_t*)malloc(Vehicle::DecodeTable::BLOB_MAX);
        cs.decodeBlob = blob;
        cs.decodeLen  = blob ? g_cfg.loadDecode(blob, Vehicle::DecodeTable::BLOB_MAX) : 0;
        // The panel alert rules: NVS, else the low-fuel edge alone.
        cs.alerts.factory();
        const bool alertsNvs = g_cfg.loadAlerts(cs.alerts);
        uint8_t alertsOn = 0;
        for (const Vehicle::AlertRule& r : cs.alerts.r) alertsOn += r.on && r.sig < Vehicle::SIG_COUNT;
        logf("[alert] %u rule(s) on, from %s", (unsigned)alertsOn, alertsNvs ? "NVS" : "factory");
#if FEATURE_DISPLAY_CAN
        // As a phone notification is drawn, but only while nothing holds the
        // glass: an alert never covers a menu (CarTask waits, newest wins).
        cs.alert = App::Ui::queueNotification;
        cs.glassFree = []() {
            // The ui task's own glassHeld(), which knows "held until released"
            // (glassHoldMs 0). The menu is asked live too: the published copy
            // can be one pass behind a menu that just opened.
            if (App::PanelMenu::isOpen() || App::PanelMenu::panelOff()) return false;
            return !App::Ui::status().read().glassHeld;
        };
#endif
        App::Car::begin(cs);
        free(blob);
        logf("[cfg] NVS: %d entries of 32 B free", g_cfg.nvsFreeEntries());
        App::Car::start();
#if FEATURE_CAN2
        // INT notifies the car task from here on; until now it only set a flag,
        // which the task's first wake (20 ms at most) finds.
        Mcp2518Source::instance().setNotifyTask(App::Car::task());
#endif
        memMark("car");
    }
#if FEATURE_DISPLAY_CAN
    // The render policy's side through copies, staged settings and asks: the
    // live screen and pane are the ui task's, and httpd never touches them.
    {
        const App::Ui::Words w = App::Ui::words();
        WebUi::UiView v;
        v.settings     = &App::Ui::settings();
        v.paneBits     = &App::Ui::paneBits();
        v.status       = &App::Ui::status();
        v.media        = &App::Ui::mediaMirror();
        v.pane         = &App::Ui::paneMirror();
        v.splashNow    = App::Ui::splashNow;
        v.releaseGlass = App::Ui::releaseGlass;
        v.manualClock  = App::Ui::noteManualClock;
        v.keyPopup     = w.keyPopup;
        v.keyPopupMs   = w.keyPopupMs;
        v.glassHoldMs  = w.glassHoldMs;
        g_web.attachUi(v);
    }
    // The panel through its snapshot only: httpd never calls the library.
    g_web.attachDisplay(&App::Panel::statusSnapshot(), &g_canRx, &g_panelOps);
    g_web.attachAffaKeys(&g_affaMap, &g_affaKeys, &g_keyLast);
#if FEATURE_BLE
    g_web.attachPhone([]() { return Bluetooth::IsConnected(); }, &g_media);
    g_web.attachHuMedia(&g_hu);
    g_web.attachHuDialect(&g_huDialect);
    g_web.attachBle([]() { return Bluetooth::GetStatusJson(); },
                    []() { Bluetooth::ClearBonds(); });
    // Null for a sink not in the chain, so /api/health (keys.dest) can report it as not live.
    g_web.attachRouting(g_amsSinkLive ? &g_amsKeys : nullptr,
                        g_hidSinkLive ? &g_hidKeys : nullptr);
#if FEATURE_ANCS
    g_web.attachHealth(&g_ancsCount, &g_engineEvents);
#else
    g_web.attachHealth(nullptr, &g_engineEvents);
#endif
#else
    // No BLE compiled in: report no phone.
    g_web.attachPhone([]() { return false; }, &g_media);
    // The head-unit source is attached in both branches.
    g_web.attachHuMedia(&g_hu);
    g_web.attachHuDialect(&g_huDialect);
#endif
#endif
#if FEATURE_VEHICLE_CAN
    g_web.attachCan(&g_sniffer);
#endif
    g_web.attachVehicle(g_state, g_emitter, g_tx, g_keySink, g_huRx);
    g_web.attachHuRing(huLastFramesJson);
#if FEATURE_DISPLAY_CAN
    g_web.attachPopupArm(App::Ui::armPopup);
    // Clear is only a count in PanelOps; the ui task acts on it.
    g_web.attachNotify(App::Ui::queueNotification, []() { g_panelOps.postClear(); });
#endif
    g_web.begin(g_cfg, g_engine, g_map, g_keySink);
    memMark("web");
    // The OTA trial's verdict comes from net_svc (Housekeeping).
    App::OtaTrial::begin();

    const char* mdns = g_wifi.mdnsHost().length() ? g_wifi.mdnsHost().c_str() : "(mdns FAILED)";
    // httpd is up: a String in Settings is read under its lock (Cfg::Config::lock).
    const String staSsid = [] { Cfg::Config::Guard g(g_cfg); return g_cfg.s().ssid; }();
    if (g_wifi.connecting())
        // WiFi no longer blocks setup(); the outcome is logged by [wifi] lines.
        logf("[MegaOpen " MEGAOPEN_VERSION "] up, WiFi joining in the background, uart=%u baud\n",
                      (unsigned)g_cfg.s().uartBaud);
    else if (g_wifi.isSta())
        logf("[MegaOpen " MEGAOPEN_VERSION "] up, STA \"%s\", http://%s/  http://%s/  uart=%u baud\n",
                      staSsid.c_str(), g_wifi.ip().c_str(), mdns,
                      (unsigned)g_cfg.s().uartBaud);
    else
        logf("[MegaOpen " MEGAOPEN_VERSION "] up, AP \"%s\", http://%s/  http://%s/  uart=%u baud\n",
                      g_wifi.apSsid().c_str(),
                      g_wifi.ip().c_str(), mdns, (unsigned)g_cfg.s().uartBaud);

    // Started after every attach(): it calls into g_web immediately. The web,
    // WiFi, the heartbeat and the OTA verdict: none of them on the
    // panel's task any more (src/app/Housekeeping.h).
    {
        App::Housekeeping::Hooks h;
        h.web      = hkWeb;
        h.wifi     = hkWifi;
        h.wifiSeen = hkWifiSeen;
        h.beat     = hkBeat;
        h.beatSlow = hkBeatSlow;
        // net_svc checks every critical task's age from its first pass, and
        // this one has been in setup() for seconds without a beat.
        App::Tasks::beat(g_loopTaskId, 0);
        App::Housekeeping::start(h);
    }
    memMark("setup end");

#if FEATURE_MATRIX
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
#endif

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
        logf("[can] state=%s txerr=%u rxerr=%u txfail=%u rxmiss=%u buserr=%u "
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
                logf("[can] FLOOD: %u frames/s dropped, queue cannot keep up. "
                     "%u of the last 24 are id %03X -- a node repeating one frame "
                     "is a node nobody is ACKing: check termination (60R across "
                     "CANH/CANL) and the transceiver's 3V3 under load",
                     (unsigned)d, (unsigned)best, (unsigned)top);
            }
        }

        // Reported only: the driver's owner recovers it (the display's poll()
        // with a panel, carLinkPump() on the car's bus).
        if (st.state == TWAI_STATE_BUS_OFF || st.state == TWAI_STATE_STOPPED)
            logf("[can] %s (the driver's owner recovers it)", kState[st.state]);
    }
#endif
#if FEATURE_DISPLAY_CAN
    App::Panel::logWindow();
#endif
    // The L2 baseline (flash erases freezing both cores): read it during a
    // log rotate. at= is the uptime of the worst gap in this window.
    logf("[stall] loopGapMax=%u us at=%u ms", (unsigned)g_stallWinMaxUs,
         (unsigned)g_stallWinAtMs);
    g_stallWinMaxUs = 0;
    g_stallWinAtMs = 0;
    // Slowest loop pass and its worst section: this window's maximum. Keys no
    // longer wait for it. The media source's mutex is the ui task's to wait
    // for now: its misses are in [ui] mediaBusy.
    logf("[loop] max=%u ms (%s %u ms)", (unsigned)(g_loopWinMaxUs / 1000),
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
// The panel bus watch's verdicts, applied on the panel task after each poll().
// FuncsReg off (passive, the radio owns the handshake): we draw over it, the
// watch only reports (health, one log line). FuncsReg on: another master on
// the bus (an OEM radio) would fight our registration, so the stack goes
// silent, passive, TX gate shut and the controller listen-only (not even an
// ACK), logged once; otherwise the TX gate opens once the listen window has
// passed.
static void panelWatchStep(uint32_t now)
{
    static bool s_silent = false, s_open = false;
    // The window starts at the first poll(): only poll() drains the receive
    // queue, so before it nothing has been heard.
    g_panelWatch.begin(now);
    if (!g_cfg.s().funcReg) {   // AP_REBOOT: fixed for this boot
        static bool s_told = false;
        if (!s_told && g_panelWatch.foreignMaster(now)) {
            s_told = true;
            LogBus::printf("[panel] another bus master on the panel bus (sync 0x%03X): FuncsReg off, "
                           "MegaOpen draws over it", (unsigned)g_panelWatch.masterId());
        }
        return;
    }
    if (g_panelWatch.foreignMaster(now)) {
        if (s_silent) return;
        s_silent = true;
        s_open = false;
        App::Panel::lib().setPassive(true);
        g_twai.setTxEnabled(false);
        g_twai.abortTx();       // frames already in the driver's queue too
        const bool lo = g_twai.setListenOnly(true);
        LogBus::printf("[panel] another bus master on the panel bus (sync 0x%03X): MegaOpen is silent "
                       "(passive, TX off, listen-only%s) until it has been quiet %u s",
                       (unsigned)g_panelWatch.masterId(), lo ? "" : " FAILED",
                       (unsigned)(MegaOpen::PanelBusWatch::REARM_MS / 1000));
        return;
    }
    if (s_silent) {
        // No foreign sync for REARM_MS: back to what the settings say; the
        // gate opens below once the listen rule allows it.
        s_silent = false;
        g_twai.setListenOnly(false);
        App::Panel::lib().setPassive(!g_cfg.s().funcReg);
        LogBus::line("[panel] no other bus master for a while: the panel stack listens, then talks again");
    }
    // Ours to open and to close: a single foreign sync shuts the gate again
    // for LISTEN_MS, before a second one would make it a master.
    const bool may = g_panelWatch.mayTransmit(now);
    if (may != s_open) {
        s_open = may;
        g_twai.setTxEnabled(may);
        // Shut: what is already queued in the driver (up to its TX depth)
        // must not go out after a foreign sync either.
        if (!may) g_twai.abortTx();
    }
}
#endif

void loop()
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
                logf("[stall] loop did not run for %u ms", (unsigned)(gapUs / 1000));
        }
    }
    g_passAtUs = g_lapAtUs = micros();
    g_passWorstUs = 0;
    g_passWorst = "-";

    // No car task (no source in this build): its pins, staged decode map and
    // test expiry are done here, so State still has one writer.
    App::Car::service(now);

#if FEATURE_VEHICLE_CAN
    // No display only; the display link drains itself inside AffaDisplay::poll().
#if !FEATURE_DISPLAY_CAN
    carLinkPump(now);
#endif
    // A web reset/baseline lands here when no frame arrives to apply it,
    // unless the car task owns the table (it applies them itself).
    if (!App::Car::running()) g_sniffer.applyRequests();
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
            logf("[loop] STALL %u ms, %u ms of it in %s", (unsigned)(passUs / 1000),
                 (unsigned)(g_passWorstUs / 1000), g_passWorst);
        App::Tasks::beat(g_loopTaskId, passUs);
    }

    // [stall] starts counting here: the pass is over, only the sleep is left.
    g_loopDoneAtUs = esp_timer_get_time();
    loopWait();
}
