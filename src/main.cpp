// The wiring: the shared objects (the settings, the car's State, the web,
// WiFi, the per-ID table), and setup() as the order things start in. Each
// module owns its objects and its task's work (docs/design/main-split.md):
//   app/LogSetup, app/BootLog   the log's sinks, the boot and crash evidence
//   app/KeyPath                 every key object; the keys and scan tasks' hooks
//   app/HuLink                  the head unit's UART, frames and ring
//   app/Media                   now-playing (head unit, phone)
//   app/LoopTask                the loop: TWAI, the panel's construction, the pass
//   app/RuleActions             what a rule does, from the car task
//   app/NetHooks                net_svc: web, WiFi, the heartbeat lines
//   ble/BleService              the phone side and ble_svc
#include <Arduino.h>
#include "Features.h"
#include "Version.h"
#include "board/BoardProfile.h"
#include "cfg/Config.h"
#include "cfg/CarStore.h"
#include "proto/canbox/Protocol.h"
#include "vehicle/VehicleState.h"
#include "net/WebUi.h"
#include "net/WifiManager.h"
#include "net/LogPush.h"
#if FEATURE_IMPROV
#include "net/ImprovSerial.h"
#endif
#include "link/AppLink.h"
#include <WiFi.h>          // AppLink's "WiFi online" answer reads the station's status
#include "utils/Log.h"
#include "utils/LogBus.h"
#include "utils/HeapSentinel.h"
#include "utils/AllocWatch.h"
#include "app/Supervisor.h"
#include "app/Rt.h"
#include "app/BootLog.h"
#include "app/LogSetup.h"
#include "app/KeyBus.h"
#include "app/KeysTask.h"
#include "app/KeyPath.h"
#include "app/HuTask.h"
#include "app/HuLink.h"
#include "app/Media.h"
#include "app/LoopTask.h"
#include "app/RuleActions.h"
#include "app/CarTask.h"
#include "app/CarDiag.h"
#include "app/NetHooks.h"
#include "app/OtaTrial.h"
#if FEATURE_GH_OTA || FEATURE_LOG_PUSH
#include "net/NetJob.h"
#endif
#if FEATURE_CAPTURE
#include "capture/CanRecorder.h"
#endif
#if FEATURE_VEHICLE_CAN
#include "vehicle/CanSniffer.h"
#include "bus/TwaiVehicleSource.h"
#endif
#if FEATURE_KNOBS
#include "knobs/KnobService.h"
#endif
#if FEATURE_CAN2
#include "bus/Mcp2518Source.h"
#endif
#if FEATURE_BLE
#include "ble/BleHub.h"
#include "ble/BleService.h"
#include "bluetooth.h"
#endif
#if FEATURE_DISPLAY_CAN
#include "app/PanelOwner.h"
#include "app/UiTask.h"
#include "app/PanelMenu.h"
#endif

using App::Boot::memMark;

static Cfg::Config           g_cfg;
#if FEATURE_KNOBS
// The two knobs and their LEDs. A source: its events go onto the KeyBus.
static Knobs::Service        g_knobs;
#endif
static Vehicle::State        g_state;
static WebUi                 g_web;
static WifiManager           g_wifi;

// Task registry ids (src/app/TaskRegistry.h): each task beats its own, and the
// [task] heartbeat line and the OTA verdict read them all. -1 = not running.
static int g_logbusTaskId = -1;

#if FEATURE_VEHICLE_CAN
static Vehicle::CanSniffer g_sniffer;
#endif

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
    AllocWatch::begin();   // counts only, from the first allocation on
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

    // What net_svc's hooks read (the settings, the web, WiFi): addresses only.
    App::NetHooks::bind(g_cfg, g_web, g_wifi);
    // Logging first: every line from here on goes through the LogBus.
    App::LogSetup::sinks();
#if FEATURE_GH_OTA || FEATURE_LOG_PUSH
    NetJob::begin();   // the GitHub jobs' worker, before anything can post one
#endif
#if FEATURE_LOG_PUSH
    App::LogSetup::push(App::NetHooks::logPushSnapshot);
#endif
    // Below the loop task (raised in setup()'s tail), on the core WiFi lives on.
    LogBus::startTask(App::Plan::LOGBUS_PRIO, App::core(App::Plan::LOGBUS_CORE), App::Plan::LOGBUS_STACK);
#if FEATURE_CAPTURE
    // The CAN recorder, after the flash log has mounted littlefs (they share
    // it). The car task pushes into it from its first frame.
    Capture::begin();
    // The spill policy and «зберегти» ask whether the car stands (speed, handbrake).
    Capture::setVehicle(&g_state);
#endif
    App::LogSetup::onShutdown();
    App::Loop::registerTask();
    g_logbusTaskId = App::Tasks::add("logbus", false);
    App::Tasks::setHandle(g_logbusTaskId, LogBus::task());
    // The supervisor runs on this tick, not on net_svc, which it watches.
    LogBus::onWake([](uint32_t busyUs) {
        App::Tasks::beat(g_logbusTaskId, busyUs);
        App::Supervisor::tick(millis());
    });
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
        d.app    = &App::Media::app();
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
    // net_svc (App::NetHooks), which stores them like any other setting.
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
        m.address = App::NetHooks::addressText;
        m.showAddress = [] {
            char head[40], body[40];
            App::NetHooks::addressText(head, sizeof(head), body, sizeof(body));
            App::Ui::queueNotice(head, body);
            LOGI("menu", "address on the panel: %s / %s\n", head, body);
        };
        m.wifiJoin = [] { g_wifi.joinNow(); };
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
#if FEATURE_IMPROV
    ImprovSerial::begin(g_wifi);
#endif
    memMark("wifi");
    g_web.attachWifi(&g_wifi);
    g_web.attachCounters(App::KeyPath::ghosts(), App::KeyPath::encSteps());
    {
        // What the web edits and another task applies (Staged<T>, app/KeyPath.h).
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
    App::NetHooks::attachKnobs(&g_knobs);
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
    g_web.attachAppMedia(&App::Media::app());
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
    g_web.attachAppMedia(&App::Media::app());
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
    AppLink::begin(g_wifi.apSsid().c_str());
    AppLink::setWifiOnline([]() { return g_wifi.isSta() && !g_wifi.connecting() && WiFi.status() == WL_CONNECTED; });
    // wifiAuto: WiFi off only for a USB client that carries the board's
    // internet (MegaOpen Companion's "net"); BLE never turns it off
    // (AppLink.h). The WebUSB page has none: with WiFi off under it the
    // board had neither, no log push, no update check.
    g_wifi.setLinkAlive(AppLink::usbNet);
#if FEATURE_LOG_PUSH
    g_wifi.setUplinkProbe(LogPush::wifiNoAnswer);
#endif
    memMark("applink");
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

    App::NetHooks::start();
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
