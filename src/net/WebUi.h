#pragma once
#include <PsychicHttp.h>
#include <ArduinoJson.h>
#include "../Features.h"
#include "RequestGate.h"
#include "gen/PageGz.h"      // the pages, gzipped at build time by tools/gen_pages.py

// Every JSON handler answers through this. A String whose allocation failed has
// a NULL buffer, and PsychicResponse::setContent() strlen()s it and reboots.
static inline int replyJson(PsychicRequest* r, const String& out)
{
    if (!out.c_str()) return r->reply(503, "text/plain", "no memory");
    return r->reply(200, "application/json", out.c_str());
}

// A page gzipped at build time (tools/gen_pages.py -> src/net/gen/PageGz.cpp).
// A page in flight holds ~8 kB of lwIP send buffers per socket; gzip cuts the
// bytes and the time to a quarter.
static inline int replyPage(PsychicRequest* r, const uint8_t* gz, size_t len)
{
    PsychicResponse res(r);
    res.setCode(200);
    res.setContentType("text/html");
    res.addHeader("Content-Encoding", "gzip");
    res.setContent(gz, len);
    return res.send();
}
#include "Catalog.h"
#include "../proto/canbox/HuDialect.h"
#include "../display/PanelBusWatch.h"

// A JSON body streamed in chunks from one static buffer rather than built as
// one String: httpd serves one request at a time. /api/catalog, /api/logs?since=,
// /api/map. s_buf is defined in WebUi.cpp.
class ChunkOut final : public Catalog::Out
{
public:
    explicit ChunkOut(PsychicRequest* r) : _res(r)
    {
        _res.setContentType("application/json");
        _res.sendHeaders();
    }
    void put(const char* s, size_t len) override
    {
        while (len) {
            if (_n == sizeof(s_buf)) flush();
            const size_t k = len < sizeof(s_buf) - _n ? len : sizeof(s_buf) - _n;
            memcpy(s_buf + _n, s, k);
            _n += k; s += k; len -= k;
        }
    }
    void put(const char* s) { put(s, strlen(s)); }
    esp_err_t finish()
    {
        flush();
        // sendChunk has already answered 500 and closed the chunking on error.
        return _err == ESP_OK ? _res.finishChunking() : _err;
    }

private:
    void flush()
    {
        if (_n && _err == ESP_OK) _err = _res.sendChunk(s_buf, _n);
        _n = 0;
    }
    static uint8_t s_buf[512];
    PsychicResponse _res;
    esp_err_t _err = ESP_OK;
    size_t _n = 0;
};
#if FEATURE_DISPLAY_CAN
#include "../display/AffaPort.h"
#include "../display/PanelOps.h"
#include "../display/PanelStatus.h"
#include "../utils/Snapshot.h"
#include "../display/MediaScreen.h"
#include "../display/MediaPane.h"
#include "../keys/AffaMap.h"
#include "../keys/AffaKeySource.h"
#include "../keys/KeyDests.h"
#include "../media/MediaRouter.h"
#include "../media/HuMediaSource.h"
#include "../app/UiTask.h"
#include "../utils/Mirror.h"
#endif
#include "../cfg/Config.h"
#include "../cfg/SettingsJson.h"
#include "WifiManager.h"
#include "../keys/KeyEngine.h"
#include "../keys/MacroEngine.h"
#include "../sinks/CanboxUartSink.h"
#include "../sinks/UartTx.h"
#if FEATURE_RESISTIVE_KEYS
#include "../sinks/ResistiveKeySink.h"
#endif
#if FEATURE_BLE
#include "../sinks/BleKeyProxy.h"
#endif
#include "../app/KeysTask.h"
#include "../utils/Staged.h"
#if FEATURE_KNOBS
#include "../knobs/KnobService.h"
#endif
#if FEATURE_CAN2
#include "../bus/Mcp2518Source.h"
#endif
#include "../vehicle/VehicleState.h"
#include "../proto/canbox/Rav4Emitter.h"
#include "../proto/canbox/RaiseFramer.h"
#if FEATURE_VEHICLE_CAN
#include "../vehicle/CanSniffer.h"
#endif

// HTTP surface: the page, the JSON API and OTA. Holds no state of its own.
//
// It runs on httpd (core 0, priority 2), never on a task that owns anything a
// key or the head-unit link uses (docs/design/event-tasks.md, Step 2): it posts
// keys to the KeyBus and frames to the hu task, stages its edits for the owner
// to apply (Staged<T>), and asks the hu task for answers (Request/Reply). It
// never takes the UART mutex and never runs the sink chain.
class WebUi {
public:
    struct Stats {
        uint32_t (*ghosts)(void*) = nullptr;
        void* ctx = nullptr;
    };

    void begin(Cfg::Config& cfg, Keys::KeyEngine& engine, Keys::KeyMap& map,
               Sinks::CanboxUartSink& uart);
    void loop();   // ElegantOTA's reboot after an upload

    // Separate so the keypad page works with no CAN transceiver. The canbox
    // console sends 0x20 key frames and reports what the head unit sent back.
    void attachVehicle(Vehicle::State& state, Canbox::Rav4Emitter& emitter, UartTx& tx,
                       Sinks::CanboxUartSink& keySink, Canbox::RxParser& huRx) {
        _state = &state; _emitter = &emitter; _tx = &tx;
        _keySink = &keySink; _huRx = &huRx;
    }

#if FEATURE_DISPLAY_CAN
    // The OEM display. httpd renders through PanelOps (the library takes a
    // render from any task) and reads the status the panel task publishes
    // after every poll(), never the library's own observers.
    void attachDisplay(const Snapshot<MegaOpen::PanelStatus>* status, const uint32_t* canRx,
                       MegaOpen::PanelOps* ops) {
        _panel = status; _canRx = canRx; _ops = ops;
    }

    // The stalk mapping, the source that reports codes with no row, and the
    // last key as the keys task resolved it (a snapshot, copied).
    void attachAffaKeys(Keys::AffaMap* map, Keys::AffaKeySource* src,
                        const Snapshot<Keys::PanelKeyLast>* last) {
        _affaMap = map; _affaSrc = src; _keyLast = last;
    }

    // The two BLE key sinks, for /api/health keys.dest. Null when BLE is off, and the
    // endpoint reports that rather than accepting a mask for a missing sink.
    // Read only: their masks are staged (attachStaging) and the keys task sets them.
#if FEATURE_BLE
    void attachRouting(Sinks::BleKeyProxy* ams, Sinks::BleKeyProxy* hid) {
        _amsSink = ams; _hidSink = hid;
    }
#endif

    // Read-only counters for /api/health, each written by one task.
    void attachHealth(const uint32_t* ancsCount, const uint32_t* keyEvents) {
        _ancsCount = ancsCount; _keyEvents = keyEvents;
    }

    // The render policy: the now-playing screen, the 48x48 pane, the splash,
    // the clock push and the glass hold, all the ui task's (App::Ui, Step 4).
    // httpd never touches the live objects: it edits the staged settings
    // (applied by the ui task on its next pass), reads the ui's counters, and
    // composes its previews from the copies the ui refreshes every pass.
    // The key popup and the glass-hold length are single-writer words (httpd
    // writes, the keys and ui tasks read), as since Step 2.
    struct UiView {
        Staged<App::Ui::MediaSettings>*      settings = nullptr;
        Staged<App::Ui::PaneBits>*           paneBits = nullptr;   // the Custom picture
        const Snapshot<App::Ui::UiStatus>*   status   = nullptr;
        const Mirror<MegaOpen::MediaScreen>* media    = nullptr;
        const Mirror<MegaOpen::MediaPane>*   pane     = nullptr;
        void (*splashNow)()    = nullptr;   // fire the track splash now
        void (*releaseGlass)() = nullptr;   // hand the glass back to the repaints
        void (*manualClock)(uint16_t hm) = nullptr;   // /api/display/clock posted this
        bool*     keyPopup    = nullptr;
        uint16_t* keyPopupMs  = nullptr;
        uint32_t* glassHoldMs = nullptr;
    };
    void attachUi(const UiView& v) { _ui = v; }

    // Phone connected, read-only, for /api/media status.
    using PhoneStateFn = bool (*)();
    void attachPhone(PhoneStateFn connected, MediaRouter* router) {
        _phoneConnected = connected; _router = router;
    }

    // The head unit as a media source, changeable live, without a reboot.
    void attachHuMedia(HuMediaSource* hu) { _hu = hu; }
    // Which canbox profile the head unit speaks (health `hu`); hu task writes.
    void attachHuDialect(const Canbox::HuDialect* d) { _huDialect = d; }
    // Who else is on the panel bus (health `panel`).
    void attachPanelWatch(const MegaOpen::PanelBusWatch* w) { _panelWatch = w; }

    // BLE status string and bond reset. Function pointers so WebUi compiles
    // with FEATURE_BLE off (bluetooth.h drags NimBLE in).
    using BleStatusFn = String (*)();
    using BleUnbondFn = void (*)();
    void attachBle(BleStatusFn status, BleUnbondFn unbond) {
        _bleStatus = status; _bleUnbond = unbond;
    }
#endif

#if FEATURE_VEHICLE_CAN
    // The loop task owns the controller. Its loopback test is a Request/Reply
    // call into Diag::CanBench, and its counters a snapshot from there.
    void attachCan(Vehicle::CanSniffer* sniffer) { _sniffer = sniffer; }
#endif

#if FEATURE_CAN2
    // CAN A (/api/can2): the car task's snapshot, and a hand-set rate passed
    // to it (0 = find it). The SPI is the car task's; httpd only reads copies.
    using Can2RateFn = void (*)(uint32_t bps);
    void attachCan2(const Snapshot<Mcp2518Source::Status>* status, Can2RateFn setRate) {
        _can2 = status; _can2Rate = setRate;
    }
#endif

#if FEATURE_RESISTIVE_KEYS
    // KEY1/KEY2: the sink, read only (its live state, for /api/res/live).
    // The ohms table and the learn hold go through Staging: the keys task owns
    // both the sink and the map.
    void attachResistive(const Sinks::ResistiveKeySink* sink) { _res = sink; }
#endif

#if FEATURE_KNOBS
    // Front-panel knobs. Talk only to the Service: the decoder and lights belong
    // to the knob task.
    void attachKnobs(Knobs::Service* k) { _knobs = k; }
#endif

    // The head-unit UART ring as JSON; main.cpp owns the storage.
    using HuDumpFn = String (*)(bool inOnly);
    void attachHuRing(HuDumpFn f) { _huDump = f; }

    // Arm the popup's take-down `ms` from now. The deadline is the loop's
    // render state; this only asks for it.
    using ArmPopupFn = void (*)(uint32_t ms);
    void attachPopupArm(ArmPopupFn f) { _armPopup = f; }
    // /api/display/notify and /api/display/clear.
    using NotifyFn = void (*)(const char* sender, const char* text);
    using ClearFn  = void (*)();
    void attachNotify(NotifyFn n, ClearFn c) { _notify = n; _clear = c; }

    // The configs the web edits and another task applies (Staged<T>, §3.1).
    // The routes write the staged copy and save it to NVS; the owner swaps it
    // in on its next pass.
    struct Staging {
        Staged<App::KeysTask::Routing>*   routing = nullptr;   // the keys task
        Staged<App::KeysTask::KeyMapCfg>* keymap  = nullptr;   // the scan task, or keys
        Staged<Keys::MacroTable>*         macros  = nullptr;   // the keys task
#if FEATURE_DISPLAY_CAN
        Staged<Keys::AffaMap>*            affa    = nullptr;   // the keys task
#endif
        // The keys task's volume fallback is on (Keys::volumeToAms). Any task.
        bool (*volToAms)() = nullptr;
        // Ask the keys task to let go of every ladder rung (and end a learn).
        void (*resAllOff)() = nullptr;
#if FEATURE_RESISTIVE_KEYS
        // The ladder's ohms, rungs and timing: the keys task applies them, and
        // releases every rung as it does (a held key must not resolve its
        // Release against a different rung).
        Staged<Sinks::ResistiveMap>*      resMap  = nullptr;
        // A learn hold, run by the keys task (it drives the rung pins).
        void (*resLearn)(const App::KeysTask::ResLearn& q) = nullptr;
#endif
    };
    void attachStaging(const Staging& s) { _st = s; }

    // The scan brackets the radio mode, which only WifiManager does safely next to BLE.
    void attachWifi(WifiManager* w) { _wifi = w; }

    void attachCounters(const uint32_t* ghosts, const uint32_t* encSteps) {
        _ghosts = ghosts; _encSteps = encSteps;
    }

    // Not feature-gated: main.cpp's executor reports effects this build lacks.
    void attachMacros(Keys::MacroEngine* m) { _macros = m; }

    PsychicHttpServer& server() { return _server; }

    // /api/display's body.
    void fillDisplay(JsonObject d);
    // For modules with their own endpoints. Setup only: the table is not locked.
    void addRoute(const char* path, http_method method, PsychicHttpRequestCallback fn);

private:
    void routes();

    // ---- Route table ----
    // Each _server.on() costs ~345 B of heap (34.5 kB for all routes). Instead
    // the routes live in this BSS table (~3 kB) and the not-found handler walks
    // it. ElegantOTA's /update is still registered normally and matches first.
    // Sized to the largest build plus headroom: t2can registers 102 (ui-v2
    // slice 8). A route past the end is counted and logged at boot, not served.
    struct Route {
        const char*                 path;
        http_method                 method;
        PsychicHttpRequestCallback  fn;
    };
    static constexpr uint8_t ROUTE_MAX = 112;
    Route   _routes[ROUTE_MAX];
    uint8_t _nRoutes = 0;
    uint8_t _routesLost = 0;      // registrations past ROUTE_MAX, logged by begin()
    void route(const char* path, http_method method, PsychicHttpRequestCallback fn);
    esp_err_t dispatch(PsychicRequest* r);
#if HEAP_HUNT
    void startHeapHunt();
#endif
    void routesCanbox();
    void routesMap();            // WebUiMap.cpp
    void routesMacros();         // WebUiMacro.cpp
#if FEATURE_RESISTIVE_KEYS
    void routesRes();            // WebUiRes.cpp
    // The keypad's buttons, best-separated rungs first. Shared by the config
    // document and auto-assign so they cannot disagree.
    uint8_t resWantedActions(Keys::Action* out, uint8_t cap) const;
#endif
#if FEATURE_KNOBS
    void routesKnobs();          // WebUiKnobs.cpp
#endif
#if FEATURE_DISPLAY_CAN
    void routesDisplay();
    void routesAffaMap();
    void routesMedia();
#endif
#if FEATURE_VEHICLE_CAN
    void routesCan();
#endif
#if FEATURE_CAN2
    void routesCan2();
#endif
#if FEATURE_CAPTURE
    void routesCap();            // WebUiCap.cpp
#endif
#if FEATURE_GH_OTA
    void routesGhOta();          // WebUiGhOta.cpp
    void gateLocalOta();         // after ElegantOTA.begin(): one OTA owner
#endif
    // Host and X-MegaOpen (RequestGate.h), for dispatch() and the OTA filter.
    RequestGate::Why gateWhy(PsychicRequest* r);
    // The mDNS name at boot: still announced after a rename until a restart.
    char _bootName[33] = {0};
    void gateOta();              // after ElegantOTA.begin(): its endpoints gated too

    PsychicHttpServer      _server;
    Cfg::Config*           _cfg = nullptr;
    Keys::KeyEngine*       _engine = nullptr;
    Keys::KeyMap*          _map = nullptr;
    Sinks::CanboxUartSink* _uart = nullptr;
    Keys::MacroEngine*     _macros = nullptr;
#if FEATURE_RESISTIVE_KEYS
    const Sinks::ResistiveKeySink* _res = nullptr;
#endif
#if FEATURE_KNOBS
    Knobs::Service*        _knobs = nullptr;
#endif
    WifiManager*           _wifi = nullptr;

    const uint32_t*        _ghosts = nullptr;
    const uint32_t*        _encSteps = nullptr;
    // Ungated: the CAN diagnostics page reports it too.
    const uint32_t*        _canRx = nullptr;
#if FEATURE_DISPLAY_CAN
    const Snapshot<MegaOpen::PanelStatus>* _panel = nullptr;   // a copy, never the library

    MegaOpen::PanelOps*    _ops = nullptr;       // every render goes through here
    Keys::AffaMap*         _affaMap = nullptr;
    Keys::AffaKeySource*   _affaSrc = nullptr;
    const Snapshot<Keys::PanelKeyLast>* _keyLast = nullptr;
    // `last` as JSON, for both /api/affamap shapes.
    void keyLastJson(char* out, size_t cap) const;
#endif
    // The routing destinations, for /api/health (keys.dest).
    void putDests(JsonObject out, const App::KeysTask::Routing& k) const;
    // The registry's values (GET /api/settings; a settings file with forFile).
    void settingsOut(JsonObject out, bool forFile);
    // POST /api/settings and POST /api/config's settings: the HTTP status,
    // body in out/type. dry: check and preview only; file: skip RAM-only keys.
    // `extra`: the caller's own refusals, listed with the registry's.
    int settingsPost(JsonObjectConst d, bool dry, bool file, String& out, const char*& type,
                     const Cfg::SettingsJson::Errors* extra = nullptr);
#if FEATURE_KNOBS
    // GET /api/knobs's document, and what POST /api/knobs (or a file) sets.
    void knobsJson(const Knobs::Config& c, JsonObject out) const;
    uint16_t knobsFromJson(JsonVariantConst in, Knobs::Config& c,
                           Cfg::SettingsJson::Errors* err = nullptr) const;
#endif
#if FEATURE_BLE
    Sinks::BleKeyProxy*    _amsSink = nullptr;
    Sinks::BleKeyProxy*    _hidSink = nullptr;
#endif
    Staging                _st;
    const uint32_t*        _ancsCount = nullptr;
    const uint32_t*        _keyEvents = nullptr;
    HuDumpFn               _huDump = nullptr;
    ArmPopupFn             _armPopup = nullptr;
    NotifyFn               _notify = nullptr;
    ClearFn                _clear  = nullptr;
#if FEATURE_DISPLAY_CAN
    UiView                 _ui;
    PhoneStateFn           _phoneConnected = nullptr;
    MediaRouter*           _router = nullptr;
    BleStatusFn            _bleStatus = nullptr;
    BleUnbondFn            _bleUnbond = nullptr;
    HuMediaSource*         _hu = nullptr;
    // The newest settings (applied or not), for a reader; false when busy.
    bool mediaSettings(App::Ui::MediaSettings& out) const;
#endif
#if FEATURE_VEHICLE_CAN
    Vehicle::CanSniffer*   _sniffer = nullptr;
#endif
#if FEATURE_CAN2
    const Snapshot<Mcp2518Source::Status>* _can2 = nullptr;
    Can2RateFn             _can2Rate = nullptr;
#endif
    Vehicle::State*        _state = nullptr;
    Canbox::Rav4Emitter*   _emitter = nullptr;
    UartTx*                _tx = nullptr;
    Sinks::CanboxUartSink* _keySink = nullptr;
    Canbox::RxParser*      _huRx = nullptr;
    const Canbox::HuDialect* _huDialect = nullptr;
    const MegaOpen::PanelBusWatch* _panelWatch = nullptr;
};
