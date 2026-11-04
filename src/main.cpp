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
#include "display/PanelBusWatch.h"
#include "proto/canbox/Protocol.h"
#include "cfg/CarStore.h"
// Knob codes: the second byte is a detent count, not a press state — no release.
#include "vehicle/VehicleState.h"
#include "net/WebUi.h"
#include "net/WifiManager.h"
#include "net/IpNotice.h"
#include "net/LogPush.h"
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
#include "app/BootLog.h"
#include "app/LogSetup.h"
#include "app/KeyBus.h"
#include "app/KeysTask.h"
#include "app/HuTask.h"
#include "app/HuLink.h"
#include "app/Media.h"
#include "app/RuleActions.h"
#include "app/KeyPath.h"
#include "app/OneShot.h"
#include "app/Housekeeping.h"
#include "app/CarTask.h"
#include "app/CarDiag.h"
#include "utils/Staged.h"
#include "utils/Snapshot.h"
#include <esp_heap_caps.h>
#include <esp_core_dump.h>
#include "utils/HeapSentinel.h"
#include "utils/JsonEscape.h"
#include "app/StockRadio.h"
#include "display/PanelRole.h"
#if FEATURE_GH_OTA || FEATURE_LOG_PUSH
#include "net/NetJob.h"
#endif
#include <esp_flash.h>
#if FEATURE_LOGFS
#include <LittleFS.h>
#endif

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
#endif

#if FEATURE_BLE
#include "ble/BleHub.h"
#include "ble/HidRole.h"
#include "bluetooth.h"
#include "media/AmsMediaSource.h"
#include "sinks/BleKeyProxy.h"
#include "sinks/BleKeySinks.h"
#include "ble/BleService.h"
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
#include "utils/PsramJson.h"
#include "app/OtaTrial.h"
#include "link/UsbLink.h"

using App::Boot::memMark;

// Wiring only — every decision lives in the objects below.

static Cfg::Config           g_cfg;
#if FEATURE_KNOBS
// The two knobs and their LEDs. A source: events go through App::KeyPath::engine().dispatch().
static Knobs::Service        g_knobs;
#endif
static Vehicle::State        g_state;
static WebUi                 g_web;
static WifiManager           g_wifi;





// Task handles for the [stk] stack high-water line, which runs on net_svc
// and so must name the loop's own handle rather than ask for "this task".
static TaskHandle_t g_loopTask = nullptr;
static TaskHandle_t bleTask()
{
#if FEATURE_BLE
    return BleService::task();
#else
    return nullptr;
#endif
}

// Task registry ids (src/app/TaskRegistry.h): each task beats its own, and the
// [task] heartbeat line and the OTA verdict read them all. -1 = not running.
static int g_loopTaskId = -1;
static int g_logbusTaskId = -1;


#if FEATURE_DISPLAY_CAN
// ---- the OEM display --------------------------------------------------------
// Protocol lives in AffaDisplay; here are its ports (src/display/AffaPort.*) and
// the policy. The library owns sync, ACKs, pacing, watchdog and bus-off recovery.
static MegaOpen::ArduinoClock g_affaClock;
#endif

#if FEATURE_VEHICLE_CAN
static Vehicle::CanSniffer g_sniffer;
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



// ---- net_svc's hooks (src/app/Housekeeping.h) ---------------------------------
// WiFi usable at least once since boot: the next OTA needs it. Our own AP
// counts: the next OTA arrives through it just as well. Counting STA only
// meant an image flashed in the car (AP, no known network) was rolled back
// and rebooted 180 s after every OTA.
// The OTA trial wants a way in for the next update: WiFi up once, or the app
// on the USB link (it carries the page and the upload too).
static bool hkWifiSeen()
{
    static bool s_seen = false;
    s_seen = s_seen || WiFi.status() == WL_CONNECTED ||
             (!g_wifi.connecting() && !g_wifi.isSta() && (WiFi.getMode() & WIFI_AP)) ||
             UsbLink::alive();
    return s_seen;
}

// WiFi, then the board's address on the panel once per connection
// (src/net/IpNotice.h): "IP 192.168.1.15 / megaopen.local boot 204" on the
// home network, "AP MegaOpen-XXXX / 192.168.4.1 boot 204" on our own. Polite:
// it waits out the menu and a glass hold (App::Ui::queueNotice).
#if FEATURE_DISPLAY_CAN
// The address as the panel shows it: "IP 192.168.1.15" over the mDNS name
// (empty with mDNS off) and the boot number, or "AP MegaOpen-XXXX" over
// 192.168.4.1 and the boot number. The connect notice (net_svc) and the
// panel menu's address row (ui task; the row shows the head alone).
static void addressText(char* head, size_t hn, char* body, size_t bn)
{
    body[0] = 0;
    if (g_wifi.connecting()) {
        snprintf(head, hn, "WiFi joining");
    } else if (g_wifi.isSta() && WiFi.status() == WL_CONNECTED) {
        snprintf(head, hn, "IP %s", WiFi.localIP().toString().c_str());
        snprintf(body, bn, "%s", g_wifi.mdnsHost().c_str());
    } else if (WiFi.getMode() & WIFI_AP) {
        snprintf(head, hn, "AP %s", g_wifi.apSsid().c_str());
        snprintf(body, bn, "%s", WiFi.softAPIP().toString().c_str());
    } else {
        snprintf(head, hn, "WiFi down");
    }
    // The boot number after the name or address ("megaopen.local boot 204"):
    // read off the panel for a report, it names the pushed log's folder
    // (boot-00204). Its own row when the first is full (wrapTwoEllipsis).
    const size_t n = strlen(body);
    snprintf(body + n, bn - n, "%sboot %lu", n ? " " : "", (unsigned long)MegaOpen::bootNo());
}
#endif
#if FEATURE_LOG_PUSH
// The status snapshot LogPush sends to GitHub every LogPush::SNAP_MS:
// /api/status and /api/health in one object, for tools rather than regexes
// (docs/design/logging.md §5). On the push task; the pools in PSRAM, beside
// its TLS handshake.
static size_t logPushSnapshot(char* out, size_t cap)
{
    // dispatch() holds the settings lock around every route; the builders
    // read Strings in Settings (bleMode), so this takes it too (recursive).
    Cfg::Config::Guard g(g_cfg);
    JsonDocument d(&PsramJson::instance());
    d["uptimeMs"] = millis();
    g_web.statusJson(d["status"].to<JsonObject>());
    g_web.healthJson(d["health"].to<JsonObject>());
    const size_t n = serializeJson(d, out, cap);
    return n < cap ? n : 0;
}
#endif

static void hkWifi(uint32_t now)
{
    g_wifi.loop(now);
#if FEATURE_LOG_PUSH
    // Online: on WiFi with internet, or an app on the USB link that offers its own.
    // Not on an OTA trial: a push takes internal heap down to a few kB, under
    // the trial's healthy line (OtaTrial::HEALTH_HEAP_MIN), and each dip starts
    // its 60 s again, so a good image rolled back. The lines wait in the spool
    // and the flash, and go once the trial has confirmed.
    const bool trial = App::OtaTrial::onTrial();
    static bool s_trialSaid = false;
    if (trial && !s_trialSaid) { s_trialSaid = true; LOGI("push", "held until the OTA trial confirms"); }
    LogPush::tick(now, !trial && ((g_wifi.isSta() && !g_wifi.connecting() && WiFi.status() == WL_CONNECTED)
                                  || UsbLink::netAvailable()));
#endif
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
    char head[40], body[40];
    addressText(head, sizeof(head), body, sizeof(body));
    App::Ui::queueNotice(head, body);
    LOGI("wifi", "on the panel: %s / %s\n", head, body);
#endif
}

// Every 10 s, after [win] and [task]: the lines nobody else owns.
static void hkBeat(uint32_t now)
{
    const uint32_t s = now / 1000;
    // Settings › Reminders, live: two bytes, read without the settings lock.
    App::Car::setReminderSpeeds(g_cfg.s().remindLightsKmh, g_cfg.s().remindBrakeKmh);
    App::Car::setGears(g_cfg.s().gearRatio);
    // [stk] is a subset of [task] now. Kept until Step 6 retires it, so old
    // logs and new ones still compare line for line.
    const TaskHandle_t net = App::Housekeeping::task();
    // httpd: the web server's own task (4 kB), where every route runs; found
    // by name once it exists (the server starts after this line's first pass).
    static TaskHandle_t s_httpd = nullptr;
    if (!s_httpd) s_httpd = xTaskGetHandle("httpd");
    LOGS("stk", "loop=%u net=%u ble=%u knobs=%u httpd=%u",
         g_loopTask ? (unsigned)uxTaskGetStackHighWaterMark(g_loopTask) : 0u,
         net ? (unsigned)uxTaskGetStackHighWaterMark(net) : 0u,
         bleTask() ? (unsigned)uxTaskGetStackHighWaterMark(bleTask()) : 0u,
#if FEATURE_KNOBS
         (unsigned)g_knobs.stackFree(),
#else
         0u,
#endif
         s_httpd ? (unsigned)uxTaskGetStackHighWaterMark(s_httpd) : 0u);
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
        LOGS("can2", "live=%u ab=%s bps=%lu fps=%lu frames=%lu hwOvf=%lu ringPeak=%lu/256 "
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
            LOGI("can2", "%lu saved: tried first at the next boot", (unsigned long)c.detected);
        }
    }
#endif
#if FEATURE_BLE
    // The BLE key path since boot: q = the keys-to-ble_svc queue's drops;
    // ams = AMS writes that went out, fail = ones the stack refused;
    // stale = waited over 300 ms; coal = volume presses folded into one
    // pending count; nolink = owed with no phone connected; vol = AMS
    // commands a volume press sends (measured, sinks/AmsVolumeStep.h).
    LOGS("ble", "keys q=%u ams=%u fail=%u hid=%u stale=%u coal=%u nolink=%u vol=%u",
         (unsigned)Sinks::BleKeyQueue::dropped(), (unsigned)BleService::keys().amsWrites(),
         (unsigned)BleService::keys().amsFailed(),
         (unsigned)BleService::keys().hidPresses(), (unsigned)BleService::keys().stale(),
         (unsigned)BleService::keys().coalesced(), (unsigned)BleService::keys().noLink(),
         (unsigned)BleService::keys().volSteps());
#endif
#if FEATURE_DISPLAY_CAN || FEATURE_BLE
    // lockMiss = head-unit frames that found a reader holding the media
    // mutex and were applied a pass later; lost = of those, stash overflow.
    LOGS("media", "hu frames=%u text=%u lockMiss=%u lost=%u",
         (unsigned)App::Media::hu().frames(), (unsigned)(App::HuLink::decoder() ? App::HuLink::decoder()->stats().textFrames : 0),
         (unsigned)App::Media::hu().lockMisses(), (unsigned)App::Media::hu().stashDropped());
#endif
    LOGS("hb", "up=%02u:%02u:%02u heap=%u min=%u big=%u canrx=%u keys=%u tx=%u/%u",
         (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60),
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
         (unsigned)ESP.getMaxAllocHeap(),
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
         (unsigned)g_canRx,
#else
         0u,
#endif
         (unsigned)App::KeyPath::engine().events(),
         (unsigned)App::HuLink::tx().keyFrames(), (unsigned)App::HuLink::tx().vehicleFrames());
}

// Once a minute: address, SSID, BSSID and gateway, to find the board and
// confirm which network (and radio) it is really on. Not every 10 s: WiFi.SSID()
// and the toString() calls allocate, and this was the loop's String work.
static void hkBeatSlow(uint32_t)
{
    if (g_wifi.connecting()) {
        LOGI("net", "joining \"%s\"", g_wifi.curSsid().c_str());
    } else if (g_wifi.isSta()) {
        // A String whose allocation failed has a NULL buffer; strlen(NULL)
        // in vsnprintf reboots. Print "?" instead.
        auto nn = [](const char* s) { return s ? s : "?"; };
        LOGS("net", "STA ssid=\"%s\" rssi=%d bssid=%s ip=%s gw=%s mask=%s %s",
             nn(WiFi.SSID().c_str()), (int)WiFi.RSSI(), nn(WiFi.BSSIDstr().c_str()),
             nn(WiFi.localIP().toString().c_str()),
             nn(WiFi.gatewayIP().toString().c_str()),
             nn(WiFi.subnetMask().toString().c_str()),
             g_wifi.mdnsHost().length() ? nn(g_wifi.mdnsHost().c_str()) : "(no mdns)");
    } else {
        LOGI("net", "AP \"%s\" ip=%s %s", g_wifi.apSsid().c_str(),
             g_wifi.ip().c_str(),
             g_wifi.mdnsHost().length() ? g_wifi.mdnsHost().c_str() : "(no mdns)");
    }
}

#if FEATURE_VEHICLE_CAN
// On the car task, after a set of formulas is applied: the car screen's rows,
// saved by name, find their (possibly moved) signals. Under the settings lock,
// since save() on httpd writes the same fields; never waits for it (a hot
// task): false, and the car task asks again on its next pass.
static bool onSignalsRenamed()
{
    if (!g_cfg.tryLock()) return false;
    g_cfg.resolveCarSigs();
    g_cfg.unlock();
    return true;
}
#endif


void setup()
{
#if HEAP_SENTINEL
    HeapSentinel::begin();
#endif
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
    App::LogSetup::sinks();
#if FEATURE_GH_OTA || FEATURE_LOG_PUSH
    NetJob::begin();   // the GitHub jobs' worker, before anything can post one
#endif
#if FEATURE_LOG_PUSH
    App::LogSetup::push(logPushSnapshot);
#endif
    // Below the loop task (raised in setup()'s tail), on the core WiFi lives on.
    LogBus::startTask(App::Plan::LOGBUS_PRIO, App::core(App::Plan::LOGBUS_CORE), App::Plan::LOGBUS_STACK);
#if FEATURE_CAPTURE
    // The CAN recorder, after the flash log has mounted littlefs (they share
    // it). The car task pushes into it from its first frame.
    Capture::begin();
#endif
    App::LogSetup::onShutdown();
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
    App::Boot::logReason();
    App::LogSetup::report(ringGrown);

    memMark("boot");
    // Tap the system log first, so boot output reaches the browser too.
    App::LogSetup::tapIdf();
    Log::setLevel(LogLevel::INF);

    g_cfg.begin();
    // The head-unit protocol, before anything reads Canbox::activeProtocol()
    // (the settings registry, the hu task, the web, the car file).
    {
        const int hp = Canbox::Protocols::find(g_cfg.s().huProtocol.c_str());
        Canbox::Protocols::select(hp < 0 ? 0 : (uint8_t)hp);
        if (hp < 0)
            LOGW("hu", "protocol \"%s\" is not in this image: %s", g_cfg.s().huProtocol.c_str(),
                 Canbox::activeProtocol().id());
    }
    // The car's configuration (/cfg/car.json; none yet: the factory's).
    // After the protocol (its field map is the protocol's) and the flash
    // log (which mounts LittleFS).
    CarStore::begin();
    // The log's own settings, saved (docs/design/logging.md).
    App::LogSetup::apply(g_cfg.s());
    App::KeyPath::loadMap(g_cfg);
#if FEATURE_DISPLAY_CAN
    // The render policy's settings from NVS, what it draws from, and its UiQ
    // (before the panel can post a verdict to it). The task starts below,
    // when there is a panel.
    {
        App::Ui::Deps d;
        d.ops    = &App::Panel::ops();
        d.router = &App::Media::router();
        d.hu     = &App::Media::hu();
        d.engine = &App::KeyPath::engine();
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
        m.ops = &App::Panel::ops();
        m.cfg = &g_cfg;
        m.car = &g_state;
#if FEATURE_KNOBS
        // Get and set together under the settings lock (applyPending holds it),
        // like every knob read-modify-write (Knobs::Service's comment).
        m.knobsGet = [](Knobs::Config& out) { out = g_knobs.config(); return true; };
        m.knobsSet = [](const Knobs::Config& c) { g_knobs.setConfig(c); g_cfg.saveKnobs(c); };
#endif
        m.stageUi = [](const Cfg::Settings& s) {
            if (!App::Ui::stageFrom(s)) LOGI("menu", "ui settings busy: shown after the next change");
        };
        m.stageMedia = [](const Cfg::Settings& s) {
            App::Media::router().setMode(MediaRouter::modeFromStr(s.mediaSource.c_str()));
        };
        // The address row, and on Select the same notice as on connect.
        m.address = addressText;
        m.showAddress = [] {
            char head[40], body[40];
            addressText(head, sizeof(head), body, sizeof(body));
            App::Ui::queueNotice(head, body);
            LOGI("menu", "address on the panel: %s / %s\n", head, body);
        };
        App::PanelMenu::begin(m);
    }
#endif
#if FEATURE_DISPLAY_CAN
    App::KeyPath::loadPanelMap(g_cfg);
#endif
    App::KeyPath::stage(g_cfg);

    App::HuLink::beginUart(g_cfg);

    App::KeyPath::beginSinks(g_cfg);

    App::HuLink::begin(g_cfg, g_state);

    App::KeyPath::beginEngine(g_cfg);

#if FEATURE_KNOBS
    // After the macro filter: the knob task starts sampling inside begin().
    {
        Knobs::Config kc;
        g_cfg.loadKnobs(kc);
        // The dash's dimmer for «Follow dash»: the instrument illumination off
        // the car's CAN (0x645 b1), 0xFF = the lights are off; stale after 3 s.
        g_knobs.setDashSource([]() -> int16_t {
            if (!g_state.fresh(Vehicle::SIG_BACKLIGHT, millis(), 3000)) return -1;
            const int32_t v = g_state.get(Vehicle::SIG_BACKLIGHT);
            return v < 0 || v >= 0xFF ? -1 : (int16_t)v;
        });
        // Each event straight onto the KeyBus: a click as one Tap item, so a
        // full lane drops the press and its release together or not at all.
        g_knobs.begin(kc, [](const Keys::Event& e, bool pair, void*) {
            return pair ? App::KeyBus::postTap(e) : App::KeyBus::postEvent(e);
        }, nullptr);
        memMark("knobs");
        // Printed every boot: pins and enable state.
        LOGI("knobs", "S1=GPIO%d S2=GPIO%d KEY=GPIO%d pot=GPIO%d led=GPIO%d  enc=%s pot=%s led=%s x%u+%u feet cap=%umA",
             (int)Board::PIN_KNOB_S1, (int)Board::PIN_KNOB_S2, (int)Board::PIN_KNOB_KEY,
             (int)Board::PIN_KNOB_POT, (int)Board::PIN_KNOB_LED,
             kc.enc[Knobs::ENC_RIGHT].enabled ? "on" : "OFF", Knobs::potModeName(kc.potMode),
             kc.ledEnabled ? "on" : "OFF",
             (unsigned)(kc.zone[0].count + kc.zone[1].count),
             (unsigned)kc.feetCount, (unsigned)kc.maxMilliamps);
    }
#endif

#if FEATURE_DISPLAY_CAN || FEATURE_BLE
    App::Media::begin(g_cfg.s());
#endif

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

    const auto& dt = g_cfg.s().displayType;
    // The role (display/PanelRole.h): the settings' word, fixed for this boot.
    // Config::begin() has already reduced an unknown id to the emulator.
    MegaOpen::PanelRole role = MegaOpen::PanelRole::Emulator;
    MegaOpen::PanelRoles::parse(g_cfg.s().panelRole.c_str(), role);
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
    // Who else is on the bus, for health `panel` and the [panel] lines
    // (panelWatchStep). The link transmits from its first poll() in either
    // role: no listen window, no gate of ours (the library keeps its own TX
    // gate for a bus where nobody ACKs).
    g_web.attachPanelWatch(&g_panelWatch);

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
    if (role == MegaOpen::PanelRole::Extension) App::StockRadio::begin(g_cfg);

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

#if FEATURE_BLE
    BleService::begin(g_cfg.s());
#endif

    // The two core-1 tasks that write the head-unit UART. Here, with every
    // key sink registered and the parser's callbacks set: the panel cannot
    // deliver a key before loop() polls it, and the knobs' early posts wait
    // on the KeyBus for the keys task's first pass.
    App::KeyPath::start();
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
    g_web.attachCounters(App::KeyPath::ghosts(), App::KeyPath::encSteps());
    {
        // What the web edits and another task applies (see App::KeyPath::routingCfg()).
        WebUi::Staging st;
        st.routing = &App::KeyPath::routingCfg();
#if FEATURE_BLE
        st.volToAms = []() { return App::KeyPath::volToAms(); };
#endif
        st.keymap  = &App::KeyPath::keymapCfg();
        st.macros  = &App::KeyPath::macroCfg();
#if FEATURE_DISPLAY_CAN
        st.affa    = &App::KeyPath::affaCfg();
#endif
#if FEATURE_RESISTIVE_KEYS
        st.resAllOff = []() { App::KeyPath::resAllOffReq().post(1); App::KeyBus::kick(); };
        st.resMap    = &App::KeyPath::resMapCfg();
        st.resLearn  = [](const App::KeysTask::ResLearn& q) {
            App::KeyPath::resLearnReq().post(q);
            App::KeyBus::kick();
        };
#endif
        g_web.attachStaging(st);
    }
#if FEATURE_RESISTIVE_KEYS
    g_web.attachResistive(&App::KeyPath::resSink());
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
        // The decode map from the car file (none: the factory table), for
        // begin() only: the car task copies it (its rows are in PSRAM).
        Vehicle::DecodeTable decode;
        CarStore::loadDecode(decode, cs.decodeFrom);
        cs.decode = &decode;
        // The formulas (none in the file: the factory's, fuel_pct and range_calc).
        Vehicle::FormulaSet formulas;
        CarStore::loadFormulas(formulas);
        cs.formulas = &formulas;
        // Once the user signals have their names: the car screen's rows,
        // saved by name, find them.
        cs.signalsRenamed = onSignalsRenamed;
        // The rules (none in the file: the factory's three), and the hand that
        // performs their actions (popups the car task queues itself).
        Vehicle::RuleSet rules;
        CarStore::loadRules(rules);
        cs.rules = &rules;
        cs.perform = App::RuleActions::perform;
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
#if FEATURE_DIAG
        App::CarDiag::begin(&g_state);   // the switches, before the car task runs
#endif
        App::Car::begin(cs);
        App::Car::setReminderSpeeds(g_cfg.s().remindLightsKmh, g_cfg.s().remindBrakeKmh);
    App::Car::setGears(g_cfg.s().gearRatio);
        LOGI("cfg", "NVS: %d entries of 32 B free", g_cfg.nvsFreeEntries());
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
    g_web.attachDisplay(&App::Panel::statusSnapshot(), &g_canRx, &App::Panel::ops());
    g_web.attachAffaKeys(&App::KeyPath::affaMap(), &App::KeyPath::affaKeys(), &App::KeyPath::keyLast());
#if FEATURE_BLE
    g_web.attachPhone([]() { return Bluetooth::IsConnected(); }, &App::Media::router());
    g_web.attachHuMedia(&App::Media::hu());
    g_web.attachBle([]() { return Bluetooth::GetStatusJson(); },
                    []() { Bluetooth::ClearBonds(); });
    // Null for a sink not in the chain, so /api/health (keys.dest) can report it as not live.
    g_web.attachRouting(App::KeyPath::amsSinkLive() ? &App::KeyPath::amsKeys() : nullptr,
                        App::KeyPath::hidSinkLive() ? &App::KeyPath::hidKeys() : nullptr);
#if FEATURE_ANCS
    g_web.attachHealth(BleService::ancsCount(), App::KeyPath::engineEvents());
#else
    g_web.attachHealth(nullptr, App::KeyPath::engineEvents());
#endif
#else
    // No BLE compiled in: report no phone.
    g_web.attachPhone([]() { return false; }, &App::Media::router());
    // The head-unit source is attached in both branches.
    g_web.attachHuMedia(&App::Media::hu());
#endif
#endif
#if FEATURE_VEHICLE_CAN
    g_web.attachCan(&g_sniffer);
#endif
    g_web.attachVehicle(g_state, App::HuLink::emitter(), App::HuLink::tx(), App::KeyPath::keySink(), App::HuLink::decoder());
    g_web.attachHuRing(App::HuLink::lastFramesJson);
#if FEATURE_DISPLAY_CAN
    g_web.attachPopupArm(App::Ui::armPopup);
    // Clear is only a count in PanelOps; the ui task acts on it.
    g_web.attachNotify(App::Ui::queueNotification, []() { App::Panel::ops().postClear(); });
#endif
    g_web.begin(g_cfg, App::KeyPath::engine(), App::KeyPath::map(), App::KeyPath::keySink());
    memMark("web");
    // The app's link pipes to the web server: after it listens.
    UsbLink::begin(g_wifi.apSsid().c_str());
    // wifiAuto: WiFi off only for a client that carries the board's internet
    // (MegaOpen Companion's "net"). The WebUSB page has none: with WiFi off
    // under it the board had neither, no log push, no update check.
    g_wifi.setLinkAlive(UsbLink::netAvailable);
#if FEATURE_LOG_PUSH
    g_wifi.setUplinkProbe(LogPush::wifiNoAnswer);
#endif
    memMark("usblink");
    // The OTA trial's verdict comes from net_svc (Housekeeping).
    App::OtaTrial::begin();

    const char* mdns = g_wifi.mdnsHost().length() ? g_wifi.mdnsHost().c_str() : "(mdns FAILED)";
    // httpd is up: a copy under the lock, so the line is never torn.
    const auto staSsid = [] { Cfg::Config::Guard g(g_cfg); return g_cfg.s().ssid; }();
    if (g_wifi.connecting())
        // WiFi no longer blocks setup(); the outcome is logged by [wifi] lines.
        LOGI("boot", "MegaOpen " MEGAOPEN_VERSION " up, WiFi joining in the background, uart=%u baud",
                      (unsigned)g_cfg.s().uartBaud);
    else if (g_wifi.isSta())
        LOGI("boot", "MegaOpen " MEGAOPEN_VERSION " up, STA \"%s\", http://%s/  http://%s/  uart=%u baud",
                      staSsid.c_str(), g_wifi.ip().c_str(), mdns,
                      (unsigned)g_cfg.s().uartBaud);
    else
        LOGI("boot", "MegaOpen " MEGAOPEN_VERSION " up, AP \"%s\", http://%s/  http://%s/  uart=%u baud",
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
    App::KeyPath::startScan();
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
                LOGI("stall", "loop did not run for %u ms", (unsigned)(gapUs / 1000));
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
