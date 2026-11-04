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
#include "app/LoopTask.h"
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
static int g_logbusTaskId = -1;



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
         App::Loop::task() ? (unsigned)uxTaskGetStackHighWaterMark(App::Loop::task()) : 0u,
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
         (unsigned)*App::Loop::canRx(),
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
    App::Loop::registerTask();
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

#if FEATURE_VEHICLE_CAN
    App::Loop::begin(g_cfg, &g_sniffer);
#else
    App::Loop::begin(g_cfg, nullptr);
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
        cs.tap = App::Loop::carTap;
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
    // Who else is on the bus, for health `panel` and the [panel] lines
    // (panelWatchStep). The link transmits from its first poll() in either
    // role: no listen window, no gate of ours (the library keeps its own TX
    // gate for a bus where nobody ACKs).
    g_web.attachPanelWatch(&App::Loop::panelWatch());
    // The panel through its snapshot only: httpd never calls the library.
    g_web.attachDisplay(&App::Panel::statusSnapshot(), App::Loop::canRx(), &App::Panel::ops());
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
        App::Loop::beat();
        App::Housekeeping::start(h);
    }
    memMark("setup end");

#if FEATURE_MATRIX
    App::KeyPath::startScan();
#endif

    App::Loop::start();
}

void loop()
{
    App::Loop::pass();
}
