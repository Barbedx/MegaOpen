#include "WebUi.h"
#include "../cfg/SettingsJson.h"
#include "../proto/canbox/Rav4Profile.h"
#include "../proto/canbox/Rav4Catalog.h"
#include "Catalog.h"
#include "../cfg/SettingsRegistry.h"
#include "../keys/KeyDests.h"
#include "GhOta.h"
#include <ArduinoJson.h>
#include <ElegantOTA.h>
#include <WiFi.h>
#include <driver/twai.h>
#include <soc/twai_struct.h>   // TWAI.error_code_capture_reg, for /api/can/ecc
#include <driver/gpio.h>
#include "../diag/CanBench.h"
#include "../board/BoardProfile.h"
#include "../Version.h"
#include "../app/TaskPlan.h"
#include "../app/KeyBus.h"
#include "../app/HuTask.h"
#include "../app/CarTask.h"
#include "../utils/Log.h"
#include "../utils/LogBus.h"
#include "../utils/LogRing.h"
#if FEATURE_LOGFS
#include "../utils/LogFile.h"
#include <LittleFS.h>
#endif
#include <esp_heap_caps.h>    // HEAP_HUNT
#include <esp_system.h>
#include "../utils/Scratch.h"
#if FEATURE_BLE && FEATURE_ANCS
#include "../apple_notification_service.h"
#endif

using namespace Keys;

// ---- small shared helpers ---------------------------------------------------

static int hexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "7D 03 39 30" / "7d033930" -> bytes. -1 on a stray character or odd nibble
// count: a half-parsed payload would get a valid checksum over the wrong bytes.
static int parseHex(const char* s, uint8_t* out, uint8_t maxLen)
{
    uint8_t n = 0;
    int hi = -1;
    for (; *s; ++s) {
        if (*s == ' ' || *s == ',' || *s == ':' || *s == '-') continue;
        const int v = hexNibble(*s);
        if (v < 0) return -1;
        if (hi < 0) { hi = v; continue; }
        if (n >= maxLen) return -1;
        out[n++] = (uint8_t)((hi << 4) | v);
        hi = -1;
    }
    return hi >= 0 ? -1 : (int)n;
}

static void appendHex(String& s, const uint8_t* b, uint8_t n)
{
    char t[4];
    for (uint8_t i = 0; i < n; ++i) {
        snprintf(t, sizeof(t), "%02X", b[i]);
        if (i) s += ' ';
        s += t;
    }
}

static bool paramBool(PsychicRequest* r, const char* name, bool dflt = false)
{
    if (!r->hasParam(name)) return dflt;
    const String v = r->getParam(name)->value();
    return v == "1" || v == "true" || v == "on";
}

// By value, not const char*: .c_str() of PsychicHttp's temporary String dangles
// once the helper returns.
static String paramStr(PsychicRequest* r, const char* name, const char* dflt = "")
{
    return r->hasParam(name) ? r->getParam(name)->value() : String(dflt);
}

// Default given as text so it goes through the same parse as a browser value.
static uint32_t paramU32(PsychicRequest* r, const char* name, const char* dflt, int base)
{
    return (uint32_t)strtoul(paramStr(r, name, dflt).c_str(), nullptr, base);
}

// The menu's scroll byte outside the catalog's `arrows` (Catalog::arrowsKnown).
static const char kScrollWhy[] = "param scroll";   // 0, 7, 11 or 3

// One /api/health verdict: a stable code for the page, an English why for people.

// The only restarts httpd does (/api/reboot, /api/factory): the
// reply goes out first, the 300 ms lets it leave, then the restart.
static esp_err_t replyAndRestart(PsychicRequest* r, const char* json)
{
    r->reply(200, "application/json", json);
    delay(300);
    ESP.restart();
    return ESP_OK;
}


uint8_t ChunkOut::s_buf[512];

// GET /api/logs?since=N: {lines:[{n,t,l,tag,msg}], next, lost, more, dropped,
// boot}. The lines after N from the LogBus ring; `lost` lines were evicted
// before this reader got to them; `dropped` = lines the full ring refused since
// boot (LogBus::dropped()). At most 200 lines or ~8 kB a reply: `more` says the
// page should ask again at once. `boot` changes every boot: the page restarts
// at since=0 instead of reading a new boot's numbers as the old one's.
static esp_err_t logLines(PsychicRequest* r, uint32_t since)
{
    static char s_line[LogBus::MAX_LINE + 1];
    static const uint32_t s_boot = esp_random();   // first call: WiFi is up, the RNG is real
    ChunkOut out(r);
    LogBus::Cursor c = LogBus::after(since);
    uint32_t ms = 0, lost = 0, bytes = 0;
    unsigned lines = 0;
    int len = -1;
    char h[112];
    out.put("{\"lines\":[");
    while (lines < 200 && bytes < 8192 && (len = LogBus::read(c, s_line, ms, lost)) >= 0) {
        if (LogBus::serialOnly(s_line)) continue;   // its [hur] line is the same frame
        const LogLine p = LogLine::parse(s_line, (uint32_t)len);
        out.put(h, snprintf(h, sizeof(h), "%s{\"n\":%lu,\"t\":%lu,\"l\":\"%s\",\"tag\":\"%s\",\"msg\":\"",
                            lines ? "," : "", (unsigned long)(c.n - 1), (unsigned long)ms,
                            Log::LEVEL_NAMES[p.level], Log::TAG_ROWS[p.tag].tag));
        // JSON string escapes; runs of plain bytes (UTF-8 included) go as they are.
        const char* q = s_line + p.msg;
        const char* const end = s_line + len;
        while (q < end) {
            const char* run = q;
            while (q < end && (uint8_t)*q >= 0x20 && *q != '"' && *q != '\\') ++q;
            out.put(run, (size_t)(q - run));
            if (q == end) break;
            if (*q == '"' || *q == '\\') { const char e[2] = {'\\', *q}; out.put(e, 2); }
            else out.put(h, snprintf(h, sizeof(h), "\\u%04x", (unsigned)(uint8_t)*q));
            ++q;
        }
        out.put("\"}");
        bytes += (uint32_t)len;
        ++lines;
    }
    out.put(h, snprintf(h, sizeof(h), "],\"next\":%lu,\"lost\":%lu,\"more\":%s,\"dropped\":%lu,\"boot\":%lu}",
                        (unsigned long)(c.n - 1), (unsigned long)lost,
                        c.n <= LogBus::lines() ? "true" : "false",
                        (unsigned long)LogBus::dropped(), (unsigned long)s_boot));
    return out.finish();
}

void WebUi::begin(Cfg::Config& cfg, KeyEngine& engine, KeyMap& map,
                  Sinks::CanboxUartSink& uart)
{
    _cfg = &cfg; _engine = &engine; _map = &map; _uart = &uart;

    // Only ElegantOTA's handlers register with esp_http_server now (the
    // routes go through dispatch(), see Route in WebUi.h). Overflow fails
    // silently, and OTA registers last, so it is the one lost.
    _server.config.max_uri_handlers = 16;

    // Required: PsychicHttp leaves LRU purging off without ENABLE_ASYNC, and then
    // a full socket table (aborted requests) stops answering SYNs until a
    // power cycle. With it on, the oldest idle socket is dropped.
    _server.config.lru_purge_enable = true;

    // Down from 7: each socket costs ~8 kB of lwIP buffers at runtime. Measured
    // 47 kB free idle, 13.4 kB with four pages fetched in parallel.
    _server.config.max_open_sockets = 4;

    // Below the loop, on the WiFi core. HTTPD_DEFAULT_CONFIG runs the server at
    // priority 5 on either core, so every request — a 400k-sample diagnostic, a
    // JSON page, an OTA chunk — preempted the task that owns the panel and the
    // keys. On core 0 it shares with WiFi, lwIP and NimBLE instead, which are
    // all above it, and nothing on the key path is there.
    _server.config.task_priority = App::Plan::HTTPD_PRIO;
    _server.config.core_id       = App::core(App::Plan::HTTPD_CORE);

    // A phone that stops reading holds httpd in send() for this long, not the
    // default 5 s. Seconds.
    _server.config.send_wait_timeout = 2;

    // [mem] marks per step of begin() (serial only; memMark is in main.cpp).
    auto mem = [](const char* what) {
        LogBus::printf("[mem] web.%-8s free=%6u big=%6u\n", what,
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    };
    mem("start");
    {
        Cfg::Config::Guard g(*_cfg);
        snprintf(_bootName, sizeof(_bootName), "%s", _cfg->s().mdnsName.c_str());
    }
    _server.listen(80);
    mem("listen");
    routes();
    // The one handler for the whole table — see Route in WebUi.h.
    _server.onNotFound([this](PsychicRequest* r) { return dispatch(r); });
#if HEAP_HUNT
    startHeapHunt();
#endif
    mem("routes");
    // No OTA password: the web API that sets every password has none either.
    // Its endpoints pass the same gate as every route (RequestGate.h).
    ElegantOTA.begin(&_server);
    gateOta();
#if FEATURE_GH_OTA
    gateLocalOta();
#endif
    mem("ota");
    if (_routesLost)
        LogBus::printf("[web] %u route(s) did not fit the table — raise ROUTE_MAX\n", _routesLost);
}

// PsychicHttpServer keeps its endpoints protected; a pointer to that member,
// formed through a class derived from it, reads them without a cast.
namespace
{
    struct EndpointList : PsychicHttpServer {
        static constexpr std::list<PsychicEndpoint*> PsychicHttpServer::*list() { return &EndpointList::_endpoints; }
    };
}

RequestGate::Why WebUi::gateWhy(PsychicRequest* r)
{
    // mdnsName is a Settings String: read under the settings lock (recursive,
    // so dispatch() holding it already is fine).
    Cfg::Config::Guard g(*_cfg);
    const http_method m = r->method();
    return RequestGate::check(r->host().c_str(), _cfg->s().mdnsName.c_str(),
                              m == HTTP_GET || m == HTTP_HEAD, r->path().c_str(),
                              r->header(RequestGate::HEADER).c_str(), _bootName);
}

// ElegantOTA registers its own handlers: /ota/start and /ota/upload pass the
// gate (and stand down while a GitHub job owns OTA); its /update page cannot
// send the header, so it is off and /update leads to the Плата page. A
// refused request falls to onNotFound, where dispatch() says why.
void WebUi::gateOta()
{
    for (PsychicEndpoint* e : _server.*EndpointList::list()) {
        const String u = e->uri();
        if (u == "/ota/start" || u == "/ota/upload") {
            e->setFilter([this](PsychicRequest* r) {
#if FEATURE_GH_OTA
                if (GhOta::busy()) return false;
#endif
                return gateWhy(r) == RequestGate::Why::Ok;
            });
        } else if (u == "/update") {
            e->setFilter([](PsychicRequest*) { return false; });
        }
    }
}

void WebUi::route(const char* path, http_method method, PsychicHttpRequestCallback fn)
{
    if (_nRoutes >= ROUTE_MAX) { ++_routesLost; return; }
    _routes[_nRoutes++] = Route{path, method, fn};
}

#if HEAP_HUNT
// Diagnostic build only (PLATFORMIO_BUILD_FLAGS=-DHEAP_HUNT=1), from the
// 2026-10-19 httpd heap-corruption hunt, which is still open: the heap is
// walked before and after every request, and every 100 ms by its own task.
// The first one to fail aborts with what it knows in the panic message, which
// the coredump keeps.
static volatile uint32_t s_huntReqs = 0;
static char s_huntLast[48] = "";
static void huntCheck(const char* when, const char* path)
{
    if (heap_caps_check_integrity_all(true)) return;
    static char msg[112];
    snprintf(msg, sizeof(msg), "HEAP_HUNT corrupt %s %s (req %u, last %s, up %u ms)", when, path,
             (unsigned)s_huntReqs, s_huntLast, (unsigned)millis());
    LogBus::printf("[heap] %s", msg);
    LogBus::flushAndWait(500);
    esp_system_abort(msg);
}
static void huntTask(void*)
{
    for (;;) { vTaskDelay(pdMS_TO_TICKS(100)); huntCheck("idle", "-"); }
}
void WebUi::startHeapHunt()
{
    xTaskCreatePinnedToCore(huntTask, "heaphunt", 3072, nullptr, 1, nullptr, 1);
    LogBus::printf("[heap] HEAP_HUNT on: integrity walk per request and every 100 ms");
}
#endif

esp_err_t WebUi::dispatch(PsychicRequest* r)
{
    // Path without the query, method exact. Linear over ~100 rows is microseconds.
    const String path = r->path();
    const http_method m = r->method();
    // Every handler runs under the settings lock (Cfg::Config::lock): the
    // panel menu writes Settings from net_svc, and a String read or assigned
    // here while it does is a use-after-free. ElegantOTA's upload is not a
    // table route, so a long upload never holds it.
    Cfg::Config::Guard cfgLock(*_cfg);
    // Before any route: the Host, and the header on a change (RequestGate.h).
    const RequestGate::Why why = gateWhy(r);
    if (why != RequestGate::Why::Ok) {
        LogBus::printf("[web] refused %s %s: %s", http_method_str(m), path.c_str(),
                       why == RequestGate::Why::Host ? "host" : "no X-MegaOpen");
        if (why == RequestGate::Why::Host) {
            // Say where it IS reachable: the address this request came in on.
            static char w[96];
            const String ip = r->client()->localIP().toString();
            snprintf(w, sizeof(w), "%s http://%s/ http://%s.local/", RequestGate::whyText(why),
                     ip.c_str(), _cfg->s().mdnsName.length() ? _cfg->s().mdnsName.c_str() : "megaopen");
            return r->reply(403, "text/plain", w);
        }
        return r->reply(403, "text/plain", RequestGate::whyText(why));
    }
#if HEAP_HUNT
    huntCheck("before", path.c_str());
    ++s_huntReqs;
    esp_err_t res = ESP_OK;
    bool hit = false;
    for (uint8_t i = 0; i < _nRoutes && !hit; ++i)
        if (_routes[i].method == m && path == _routes[i].path) { res = _routes[i].fn(r); hit = true; }
    if (!hit) res = r->reply(404, "text/plain", "not_found");
    huntCheck("after", path.c_str());
    snprintf(s_huntLast, sizeof(s_huntLast), "%s", path.c_str());
    return res;
#else
    for (uint8_t i = 0; i < _nRoutes; ++i)
        if (_routes[i].method == m && path == _routes[i].path)
            return _routes[i].fn(r);
    return r->reply(404, "text/plain", "not_found");
#endif
}

void WebUi::loop()
{
    ElegantOTA.loop();
}

// Every destination the catalog lists (Keys::DEST_IDS), from the staged
// routing: its mask, whether it can take a key at all (`live`, and `code` when
// it cannot: the page has the words, kb.why.<code>), and `state`. For the keys
// chain of /api/health.
void WebUi::putDests(JsonObject out, const App::KeysTask::Routing& k) const
{
    auto dest = [&](Keys::Dest i, uint8_t mask, bool live, const char* code) {
        JsonObject o = out[Keys::DEST_IDS[i]].to<JsonObject>();
        o["mask"]  = mask;
        o["live"]  = live;
        o["state"] = !live ? "off" : (mask ? "ok" : "muted");
        if (!live) o["code"] = code;
    };
    // Live only while the head unit is heard: keys to a silent one go nowhere.
    // A build that cannot tell (no HuMediaSource) counts it live.
#if FEATURE_DISPLAY_CAN
    const bool heard = !_hu || _hu->heard(millis());
#else
    const bool heard = true;
#endif
    dest(Keys::DEST_UART, k.uartCls, k.uartOn && heard,
         !k.uartOn ? "uart_off" : "hu_silent");
#if FEATURE_BLE
    // `volFallback`: volume goes to the phone because nothing live takes it
    // (Keys::volumeToAms); the mask is the user's, without the fallback bit.
    const bool vol = _st.volToAms && _st.volToAms();
    dest(Keys::DEST_AMS, k.amsCls, _amsSink != nullptr, "no_ams_role");
    out[Keys::DEST_IDS[Keys::DEST_AMS]]["volFallback"] = vol;
    dest(Keys::DEST_HID, k.hidCls, _hidSink != nullptr, "no_hid_role");
#else
    dest(Keys::DEST_AMS, 0, false, "not_built");
    dest(Keys::DEST_HID, 0, false, "not_built");
#endif
#if FEATURE_RESISTIVE_KEYS
    dest(Keys::DEST_RES, k.resCls, k.resOn, "res_off");   // KEY1/KEY2 ladder
#else
    dest(Keys::DEST_RES, 0, false, "not_built");
#endif
#if FEATURE_USB_HID
    // Always in the chain; whether a host is plugged in is not known here.
    dest(Keys::DEST_USB, k.usbCls, true, "");
#endif
}

// The registry's values as GET /api/settings sends them, or (`forFile`) as a
// settings file carries them: on copies of what each owner holds now.
void WebUi::settingsOut(JsonObject d, bool forFile)
{
    auto& s = _cfg->s();
    // The newest rules, applied or not (the car task owns them).
    Vehicle::AlertRules al;
    Vehicle::AlertRules* alp = App::Car::alertRules().peek(al) ? &al : nullptr;
    // The emitter's, likewise (the hu task owns it).
    static App::Hu::EmitterCfg hu;
    const bool huOk = _emitter && App::Hu::emitterCfg().peek(hu);
#if FEATURE_CAPTURE
    Capture::Settings cs = Capture::settings();
    Capture::Settings* csp = &cs;
#else
    Capture::Settings* csp = nullptr;
#endif
#if FEATURE_KNOBS
    Knobs::Config kc;
    if (_knobs) kc = _knobs->config();
    const Cfg::Model m{&s, _knobs ? &kc : nullptr, alp, csp,
                       huOk ? &hu.tuning : nullptr, huOk ? hu.msgOn : nullptr};
#else
    const Cfg::Model m{&s, nullptr, alp, csp, huOk ? &hu.tuning : nullptr, huOk ? hu.msgOn : nullptr};
#endif
    Cfg::SettingsJson::write(m, d, forFile);
}

// POST /api/settings and the settings half of POST /api/config: check every
// key, then (unless `dry`) apply and hand each part to the task that owns it.
// `file`: a settings file, whose volatile entries are skipped. Returns the
// HTTP status; `out`/`type` are the body.
int WebUi::settingsPost(JsonObjectConst d, bool dry, bool file, String& out, const char*& type,
                        const Cfg::SettingsJson::Errors* extra)
{
        auto& s = _cfg->s();
        // The AP password, which is also the OTA password. Empty = unchanged.
        // Never from a file (SettingsJson::read refuses it there).
        const String apPass = file ? String() : String(d["apPass"] | "");
        if (apPass.length() && (apPass.length() < 8 || apPass.length() > 63))
            { out = "apPass: pass_len"; return 400; }

        // The alert rules and the emitter's settings are edited on copies of
        // what their tasks hold now (rows and cells not sent keep their
        // values); a copy goes back only when something in it changed.
        // Static: httpd serves one request at a time.
        static Vehicle::AlertRules al;
        Vehicle::AlertRules* alp = App::Car::alertRules().peek(al) ? &al : nullptr;
        static App::Hu::EmitterCfg hu;
        const bool huOk = _emitter && App::Hu::emitterCfg().peek(hu);
        static Canbox::Tuning huWas;
        huWas = hu.tuning;
#if FEATURE_CAPTURE
        Capture::Settings cs = Capture::settings();
        Capture::Settings* csp = &cs;
#else
        Capture::Settings* csp = nullptr;
#endif
#if FEATURE_KNOBS
        // Read here, set whole at the end: safe because this handler holds the
        // settings lock throughout, as the menu's knob commits do (dispatch()).
        Knobs::Config kc;
        if (_knobs) kc = _knobs->config();
        Cfg::Model m{&s, _knobs ? &kc : nullptr, alp, csp, huOk ? &hu.tuning : nullptr, huOk ? hu.msgOn : nullptr};
#else
        Cfg::Model m{&s, nullptr, alp, csp, huOk ? &hu.tuning : nullptr, huOk ? hu.msgOn : nullptr};
#endif
        // A file skips what this board does not have, so a part that is
        // merely busy (its owner held it for the peek) must not pass for
        // missing: retry before anything is read or applied.
        if (file && (!alp || (_emitter && !huOk))) { out = "busy"; return 503; }

        // Everything that can be refused is checked before anything is applied
        // (the /api/map rule): a 400 lists every refusal and leaves the
        // settings as they were. Keys the registry does not know (apPass,
        // huForget, a newer file's) are not refused. The ~5 kB of working
        // space lives only for this request, in PSRAM where there is some
        // (Scratch): not for httpd's stack, and not static (internal RAM).
        struct Work {
            Cfg::SettingsJson::Read   rd;
            Cfg::SettingsJson::Errors er;
            char err[Cfg::SettingsJson::Errors::MAX * Cfg::SettingsJson::Errors::LEN];
        };
        Scratch<Work> w;
        if (!w) { out = "no_memory"; return 503; }
        Cfg::SettingsJson::Read& rd = w->rd;
        Cfg::SettingsJson::Errors& er = w->er;
        Cfg::SettingsJson::read(m, d, rd, er, file);
        // The caller's own refusals (a file's knobs), in the same list.
        if (extra) {
            for (uint8_t i = 0; i < extra->n; ++i) er.add("%s", extra->line[i]);
            er.more = (uint16_t)(er.more + extra->more);
        }
        if (er.any()) {
            er.join(w->err, sizeof(w->err));
            out = w->err;
            return 400;
        }

        // A dry run: what would change, nothing applied (the table copies are
        // this handler's own).
        if (dry) {
            JsonDocument o;
            o["ok"] = true;
            bool reboot = false;
            JsonArray chg = o["changes"].to<JsonArray>();
            for (uint8_t i = 0; i < rd.n; ++i)
                if (Cfg::SettingsJson::differs(m, rd.ch[i])) {
                    chg.add(rd.ch[i].e->id);
                    if (rd.ch[i].e->apply & Cfg::AP_REBOOT) reboot = true;
                }
            for (uint8_t i = 0; i < rd.nTables; ++i) chg.add(rd.tables[i]->id);
            o["reboot"]  = reboot;
            o["unknown"] = rd.unknown;
            JsonArray sk = o["skipped"].to<JsonArray>();
            for (uint8_t i = 0; i < rd.nSkipped; ++i) sk.add(rd.skipped[i]->id);
            serializeJson(o, out);
            type = "application/json";
            return 200;
        }

        // The rules first: a busy hand-off is the last refusal, so a refused
        // request has changed nothing.
        bool alSaved = true;
        if (rd.apT & Cfg::AP_CAR) {
            if (!App::Car::alertRules().stage(al)) { out = "busy"; return 503; }
            uint8_t on = 0;
            for (const Vehicle::AlertRule& x : al.r) on += x.on;
            alSaved = _cfg->saveAlerts(al);
            LogBus::printf("[alert] rules saved: %u on%s", (unsigned)on, alSaved ? "" : ", NOT in NVS");
        }

        // What the head-unit text learner had, so only a real change resets it.
        const uint8_t huT = s.huTitleCmd, huA = s.huArtistCmd;
        const bool huFirst = s.huTitleFirst;
        const Cfg::Str src = s.mediaSource;


        uint8_t ap = 0;
        Cfg::SettingsJson::apply(m, rd, &ap);
        bool restart = (ap & Cfg::AP_REBOOT) != 0;

        // The emitter, for the hu task: the tuning as a whole block (replace,
        // not merge, so a concurrent edit is never half-applied) and the
        // message switches (RAM only). The tuning goes to NVS when it changed.
        bool huBusy = false;
        if (ap & Cfg::AP_HU) {
            huBusy = !App::Hu::emitterCfg().edit([&](App::Hu::EmitterCfg& c) {
                c.tuning = hu.tuning;
                memcpy(c.msgOn, hu.msgOn, sizeof(c.msgOn));
            });
            if (huBusy) LogBus::line("settings: the head-unit emitter was busy, its part not applied");
            else if (memcmp(&hu.tuning, &huWas, sizeof(huWas))) {
                _cfg->saveTuning(hu.tuning);
                LogBus::line("canbox tuning saved (survives a power cycle)");
            }
        }

        // Staged for the tasks that own them: the debounce timing and the
        // encoder direction for the matrix sweep; the pulse, the links and
        // the class masks for the keys task.
        if (ap & Cfg::AP_KEYS) {
            if (_st.keymap) {
                const Input::KeyDebouncer::Params t = s.timing;
                const bool inv = s.encInverted;
                _st.keymap->edit([&](App::KeysTask::KeyMapCfg& k) { k.timing = t; k.encInverted = inv; });
            }
            if (_st.routing) {
                const uint16_t pulse = s.pulseMs;
                const bool on = s.uartEnabled, resOn = s.resEnabled;
                const uint8_t u = s.routeUart, a = s.routeAms, h = s.routeHid, rs = s.routeRes,
                              us = s.routeUsb;
                if (!_st.routing->edit([&](App::KeysTask::Routing& k) {
                        k.pulseMs = pulse; k.uartOn = on; k.resOn = resOn;
                        k.uartCls = u; k.amsCls = a; k.hidCls = h; k.resCls = rs; k.usbCls = us;
                    }))
                    LogBus::line("settings: the keys task's routing was busy, applied at the next change");
                LogBus::printf("routing: uart=0x%02X ams=0x%02X hid=0x%02X res=0x%02X usb=0x%02X",
                               u, a, h, rs, us);
            }
#if FEATURE_RESISTIVE_KEYS
            // A rung held under a mask just changed would never see its Release.
            if (_res && _st.resAllOff) _st.resAllOff();
#endif
            App::KeyBus::kick();
        }

#if FEATURE_DISPLAY_CAN
        // The screen, the pane and the key popup, live.
        if ((ap & Cfg::AP_UI) && !App::Ui::stageFrom(s))
            LogBus::line("settings: the screen's were busy, applied at the next change");

        // Where the now-playing text comes from: live, so sources can be
        // compared with the panel in view. Setting a command byte forgets
        // what was learned, so it is set only when it changed.
        if (ap & Cfg::AP_MEDIA) {
            if (_router && s.mediaSource != src) _router->setMode(MediaRouter::modeFromStr(s.mediaSource.c_str()));
            if (_hu && (s.huTitleCmd != huT || s.huArtistCmd != huA)) _hu->setChannels(s.huTitleCmd, s.huArtistCmd);
            if (_hu && s.huTitleFirst != huFirst) _hu->setTitleFirst(s.huTitleFirst);
        }
        // "Throw away what you learned and watch again", for when the learner
        // latched onto a status frame that happened to look like text.
        if (!file && _hu && (d["huForget"] | false)) _hu->forget();
#endif

#if FEATURE_KNOBS
        if ((ap & Cfg::AP_KNOBS) && _knobs) {
            _knobs->setConfig(kc);
            _cfg->saveKnobs(kc);
        }
#endif

#if FEATURE_CAPTURE
        // Clamped and saved to its own NVS namespace by the recorder.
        if (ap & Cfg::AP_CAP) Capture::setSettings(cs);
#endif

        if (apPass.length()) { restart = restart || apPass != s.apPass; s.apPass = apPass; }

        _cfg->save();
        LogBus::printf("settings saved: %u value(s)%s%s", (unsigned)rd.n, file ? " from a file" : "",
                       restart ? ", pending reboot" : "");
        // A file reports a part that did not take as part of a 200, so its
        // caller still applies the knobs and the page says exactly what
        // stayed behind; the form keeps its statuses.
        if (file) {
            JsonDocument o;
            o["ok"] = true;
            if (restart) o["pendingReboot"] = true;
            if (!alSaved || huBusy) {
                JsonArray p = o["partial"].to<JsonArray>();
                if (!alSaved) p.add("alerts: nvs_failed");   // applied until the next boot only
                if (huBusy)   p.add("hu_busy");
            }
            serializeJson(o, out);
            type = "application/json";
            return 200;
        }
        if (!alSaved) { out = "alerts: nvs_failed"; return 500; }   // applied until the next boot only
        if (huBusy) { out = "hu_busy"; return 503; }   // its part not applied; the rest saved
        type = "application/json";
        out = restart ? "{\"ok\":true,\"pendingReboot\":true}" : "{\"ok\":true}";
        return 200;

}

void WebUi::routes()
{
    // The app (web/, tools/gen_pages.py). Its screens are hash routes.
    route("/", HTTP_GET, [](PsychicRequest* r) {
        return replyPage(r, PAGE_APP_GZ, PAGE_APP_GZ_LEN);
    });
    // The page's English words (web/i18n/en.json); Ukrainian is in the page.
    route("/i18n/en.json", HTTP_GET, [](PsychicRequest* r) {
        return replyPage(r, PAGE_I18N_EN_GZ, PAGE_I18N_EN_GZ_LEN, "application/json");
    });

    // The page's lists (Catalog.h), ~11 kB: streamed (ChunkOut).
    route("/api/catalog", HTTP_GET, [](PsychicRequest* r) {
        ChunkOut out(r);
        Catalog::write(out);
        return out.finish();
    });

    // The log threshold is the registry's `logLevel` (RAM only, boot is INF):
    // GET/POST /api/settings, and the panel menu's Service group.

    // ?since=N: the live log (logLines). Without it, the log pipeline: ring
    // counters always; the flash files where they exist.
    route("/api/logs", HTTP_GET, [](PsychicRequest* r) {
        if (r->hasParam("since")) return logLines(r, paramU32(r, "since", "0", 10));
        JsonDocument d;
        d["lines"]    = LogBus::lines();
        d["dropped"]  = LogBus::dropped();   // non-zero: the ring overflowed
        d["ringSize"] = LogBus::ringSize();
#if FEATURE_LOGFS
        d["mounted"] = LogFile::mounted();
        d["mountError"] = LogFile::mountError();
        d["buffered"] = LogFile::buffered();   // in RAM, not yet in /log/0.log
        d["used"]    = LogFile::usedBytes();
        d["total"]   = LogFile::totalBytes();
        JsonArray files = d["files"].to<JsonArray>();
        for (int n = 0; n < LogFile::FILES; ++n) {
            if (!LogFile::path(n)) continue;
            JsonObject f = files.add<JsonObject>();
            f["n"]    = n;   // 0 = this boot, 1 = the boot before, ...
            f["size"] = LogFile::size(n);
        }
#endif
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

#if FEATURE_LOGFS
    // /api/logs/file?n=1 — one boot's log as text. n=0 is still being written.
    route("/api/logs/file", HTTP_GET, [](PsychicRequest* r) {
        const int n = (int)paramU32(r, "n", "0", 10);
        // This boot's file: an explicit dump, so the RAM buffer goes to flash
        // first (the LogBus task writes it; this waits up to 500 ms).
        if (n == 0) LogBus::flushAndWait(500);
        const char* p = LogFile::path(n);
        if (!p) return r->reply(404, "text/plain", "no_log");
        PsychicFileResponse resp(r, LittleFS, p, "text/plain", false);
        return resp.send();
    });
#endif

    route("/api/status", HTTP_GET, [this](PsychicRequest* r) {
        JsonDocument d;
        d["ip"]        = WiFi.isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
        d["mode"]      = WiFi.isConnected() ? "sta" : "ap";
        d["ssid"]      = WiFi.isConnected() ? WiFi.SSID() : WiFi.softAPSSID();
        d["rssi"]      = WiFi.RSSI();
        d["uptimeMs"]  = millis();
        d["fw"]        = MEGAOPEN_VERSION;
        d["build"]     = MegaOpen::buildId();
        d["apName"]    = _wifi ? _wifi->apSsid() : String();
        d["framesSent"]= _uart->framesSent();
        d["events"]    = _engine->events();
        d["ghosts"]    = _ghosts ? *_ghosts : 0;
        d["encSteps"]  = _encSteps ? *_encSteps : 0;
        d["heap"]      = ESP.getFreeHeap();
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

#if FEATURE_MATRIX
    // The 3x3 keypad's map (no page since ui-v2 slice 8).
    route("/api/keymap", HTTP_GET, [this](PsychicRequest* r) {
        // The newest edit, applied or not: what the page just saved.
        App::KeysTask::KeyMapCfg k;
        if (!_st.keymap || !_st.keymap->peek(k)) return r->reply(503, "text/plain", "busy");
        const KeyMap* const map = &k.map;
        JsonDocument d;
        JsonArray slots = d["slots"].to<JsonArray>();
        for (uint8_t i = 0; i < KeyMap::POSITIONS; ++i) {
            const Slot& s = map->slot(i);
            JsonObject o = slots.add<JsonObject>();
            o["pos"]       = i;
            o["action"]    = name(s.action);
            o["code"]      = s.code;
            o["effective"] = s.code ? s.code : Canbox::rav4CodeFor(s.action);
        }
        const uint8_t cw = map->encCode(true), ccw = map->encCode(false);
        d["encCw"]       = cw  ? cw  : (uint8_t)Canbox::RAV4_KNOB_TUNE_UP;
        d["encCcw"]      = ccw ? ccw : (uint8_t)Canbox::RAV4_KNOB_TUNE_DOWN;
        d["encInverted"] = k.encInverted;
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // Staged: the task that sweeps the matrix applies it (and with it the
    // encoder phases and the fast-debounce mask) at its next pass.
    route("/api/keymap", HTTP_POST, [this](PsychicRequest* r) {
        if (!_st.keymap) return r->reply(503, "text/plain", "not_built keymap");
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");

        _cfg->s().encInverted = d["encInverted"] | false;
        const bool inv = _cfg->s().encInverted;
        App::KeysTask::KeyMapCfg saved;
        const bool ok = _st.keymap->edit([&](App::KeysTask::KeyMapCfg& k) {
            for (JsonObject o : d["slots"].as<JsonArray>()) {
                uint8_t pos = o["pos"] | 0xFF;
                if (pos >= KeyMap::POSITIONS) continue;
                k.map.setSlot(pos, Slot{fromName(o["action"] | "None"),
                                        (uint8_t)(o["code"] | 0)});
            }
            k.map.setEncCode(true,  (uint8_t)(d["encCw"]  | 0));
            k.map.setEncCode(false, (uint8_t)(d["encCcw"] | 0));
            k.encInverted = inv;
            saved = k;
        });
        if (!ok) return r->reply(503, "text/plain", "busy");
        App::KeyBus::kick();
        _cfg->saveKeyMap(saved.map);
        _cfg->save();
        LogBus::line("keymap saved");
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    route("/api/keymap/defaults", HTTP_POST, [this](PsychicRequest* r) {
        if (!_st.keymap) return r->reply(503, "text/plain", "not_built keymap");
        App::KeysTask::KeyMapCfg saved;
        if (!_st.keymap->edit([&](App::KeysTask::KeyMapCfg& k) { k.map.loadDefaults(); saved = k; }))
            return r->reply(503, "text/plain", "busy");
        App::KeyBus::kick();
        _cfg->saveKeyMap(saved.map);
        LogBus::line("keymap reset to Megane defaults");
        return r->reply(200, "application/json", "{\"ok\":true}");
    });
#endif

    // ---- every setting the registry describes (src/cfg/SettingsRegistry.h) ----
    // The same table the panel menu draws: a key here is a registry id, a
    // Bool is true/false, an Int a number, an Enum its stored id. A Table is
    // a list of rows, each a list of cells in column order (the catalog's
    // `cols`), cells as above; a scaled Int has its decimal point placed.
    route("/api/settings", HTTP_GET, [this](PsychicRequest* r) {
        auto& s = _cfg->s();
        JsonDocument d;
        settingsOut(d.to<JsonObject>(), false);
        // Not settings: the network names (passwords never) and the build.
        d["ssid"]           = s.ssid;
        d["ssid2"]          = _wifi ? _wifi->net2Ssid() : String();
        // fw is the build; cfgVer is the NVS schema version, not a build version.
        d["fw"]             = MEGAOPEN_VERSION;
        d["build"]          = MegaOpen::buildId();
        d["cfgVer"]         = _cfg->version();
        d["cfgVerWas"]      = _cfg->migratedFrom();
        d["wifiSleep"]      = _wifi ? _wifi->sleeping() : false;   // derived, read-only
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    route("/api/settings", HTTP_POST, [this](PsychicRequest* r) {
        JsonDocument d;
        if (deserializeJson(d, r->body()) || !d.is<JsonObjectConst>())
            return r->reply(400, "text/plain", "bad_json");
        String out;
        const char* type = "text/plain";
        const int st = settingsPost(d.as<JsonObjectConst>(), paramStr(r, "dry") == "1", false, out, type);
        return r->reply(st, type, out.c_str());
    });

    // ---- the settings file ----------------------------------------------------
    // GET: every setting a file should carry (no read-only, no RAM-only ones)
    // and the knob configuration, with the schema and the build that wrote it.
    // POST: such a file, through the same checks as POST /api/settings, all or
    // nothing; ?dry=1 answers what would change and changes nothing.
    route("/api/config", HTTP_GET, [this](PsychicRequest* r) {
        // Streamed (ChunkOut): the settings and the knobs each as their own
        // short-lived document, never the whole file as one String.
        ChunkOut out(r);
        struct Writer {
            ChunkOut& o;
            size_t write(uint8_t c) { const char ch = (char)c; o.put(&ch, 1); return 1; }
            size_t write(const uint8_t* b, size_t n) { o.put(reinterpret_cast<const char*>(b), n); return n; }
        } w{out};
        char head[160];
        snprintf(head, sizeof(head), "{\"format\":\"%s\",\"schema\":%d,\"fw\":\"%s\",\"build\":\"%s\",\"settings\":",
                 Cfg::SettingsJson::FORMAT, Cfg::SettingsJson::SCHEMA, MEGAOPEN_VERSION, MegaOpen::buildId());
        out.put(head);
        {
            JsonDocument d;
            settingsOut(d.to<JsonObject>(), true);
            serializeJson(d, w);
        }
#if FEATURE_KNOBS
        if (_knobs) {
            out.put(",\"knobs\":");
            JsonDocument d;
            knobsJson(_knobs->config(), d.to<JsonObject>());
            serializeJson(d, w);
        }
#endif
        mapToFile(out);   // the CAN map and the head-unit emitter
        out.put("}");
        return out.finish();
    });

    route("/api/config", HTTP_POST, [this](PsychicRequest* r) {
        JsonDocument d;
        if (deserializeJson(d, r->body()) || !d.is<JsonObjectConst>())
            return r->reply(400, "text/plain", "bad_json");
        const auto chk = Cfg::SettingsJson::checkFile(d.as<JsonObjectConst>());
        if (chk != Cfg::SettingsJson::FileCheck::Ok)
            return r->reply(400, "text/plain", Cfg::SettingsJson::fileCheckText(chk));
        const bool dry = paramStr(r, "dry") == "1";

        // The knobs first, onto a copy: a name that is no row is a refusal,
        // listed with the settings' own (settingsPost's `extra`), and what
        // sanitize() had to bring into range is shown in the preview. The
        // copy is applied only after the settings were.
        JsonVariantConst kj = d["knobs"];
        Scratch<Cfg::SettingsJson::Errors> kerrBox;
        if (!kerrBox) return r->reply(503, "text/plain", "no_memory");
        Cfg::SettingsJson::Errors& kerr = *kerrBox.get();
        bool knobs = false;
        uint16_t clamped = 0;
#if FEATURE_KNOBS
        Knobs::Config kc;
        knobs = _knobs && kj.is<JsonObjectConst>();
        if (knobs) {
            kc = _knobs->config();
            clamped = knobsFromJson(kj, kc, &kerr);
        }
#endif
        // The CAN map and the head-unit emitter are checked before anything
        // is applied, like the rest; applied after the settings.
        bool mapIn = false, huIn = false;
        if (const char* why = mapFromFile(d["decode"], d["hu"], true, mapIn, huIn))
            return r->reply(400, "text/plain", why);
        String out;
        const char* type = "text/plain";
        const int st = settingsPost(d["settings"].as<JsonObjectConst>(), dry, true, out, type, &kerr);
        if (st != 200) return r->reply(st, type, out.c_str());
        if (!dry && (mapIn || huIn))
            if (const char* why = mapFromFile(d["decode"], d["hu"], false, mapIn, huIn))
                return r->reply(500, "text/plain", why);

        JsonDocument o;
        deserializeJson(o, out);
        if (dry) {
            o["knobs"] = knobs;
            o["map"]   = mapIn;      // the file carries the CAN map
            o["hu"]    = huIn;       // and the head-unit emitter
            JsonArray cl = o["clamped"].to<JsonArray>();
#if FEATURE_KNOBS
            for (uint16_t b = 1; b; b = (uint16_t)(b << 1))
                if (clamped & b) cl.add(Knobs::Config::clampName(b));
#else
            (void)cl; (void)clamped;
#endif
            o["fileFw"] = d["fw"] | "";
        }
#if FEATURE_KNOBS
        else if (knobs) {
            _knobs->setConfig(kc);
            _cfg->saveKnobs(kc);
            LogBus::line("[knobs] set from a settings file");
        }
#endif
        out = "";
        serializeJson(o, out);
        return replyJson(r, out);
    });

    // ElegantOTA's own update page cannot send X-MegaOpen, so it is off
    // (gateOta()); its address leads to the Плата page, which updates.
    route("/update", HTTP_GET, [](PsychicRequest* r) {
        // 302, not the library's redirect() (301, which a browser keeps).
        PsychicResponse resp(r);
        resp.setCode(302);
        resp.addHeader("Location", "/#board");
        return resp.send();
    });

    route("/api/reboot", HTTP_POST, [](PsychicRequest* r) {
        LogBus::line("[sys] reboot requested from the web");
        return replyAndRestart(r, "{\"ok\":true}");
    });

    // ---- does it all work together -------------------------------------------
    // Each entry is a chain with one verdict and, when not "ok", the link that
    // broke it.
    route("/api/health", HTTP_GET, [this](PsychicRequest* r) {
        JsonDocument d;
        d["fw"] = MEGAOPEN_VERSION;
        // Which flash: the version above is the same in every image.
        d["build"] = MegaOpen::buildId();
        JsonArray chains = d["chains"].to<JsonArray>();

        // "ok" it works right now · "idle" wired up, nothing to do · "off"
        // deliberately switched off · "broken" should work and does not.
        // `code` is stable and the only explanation sent: the page has the
        // words for it in both languages (web/i18n/*.json, "w.<code>").
        auto chain = [&](const char* id, const char* state, const char* code) {
            JsonObject o = chains.add<JsonObject>();
            o["id"] = id;
            o["state"] = state;
            o["code"] = code;
            return o;
        };

        // _phoneConnected (AMS/CTS link) exists only in display builds.
#if FEATURE_DISPLAY_CAN
        const bool phone = _phoneConnected ? _phoneConnected() : false;
#else
        const bool phone = false;
        (void)phone;
#endif

#if FEATURE_DISPLAY_CAN
        // The panel task's copy: httpd never asks the library itself. The ui
        // task's counters, settings and screen likewise: copies only.
        const MegaOpen::PanelStatus ps = _panel ? _panel->read() : MegaOpen::PanelStatus{};
        const App::Ui::UiStatus us = _ui.status ? _ui.status->read() : App::Ui::UiStatus{};
        App::Ui::MediaSettings ms;
        const bool haveMs = mediaSettings(ms);
        // ~320 bytes, not on the stack and not static: for this request only
        // (Scratch). No memory reads as no media; every use checks haveMedia.
        Scratch<MegaOpen::MediaScreen> box;
        const bool haveMedia = box && _ui.media && _ui.media->copy(*box);
        // 1. The panel. Registered is the health signal; PeerAlive is omitted
        // because it toggles every second (sync 0x0A and 0x08 are both healthy).
        {
            static constexpr const char* W[] = {
                "panel_off",
                "panel_unregistered",
                "panel_ok",
                "panel_foreign_master",
                "panel_cluster_only",
                "panel_beside_radio"};
            const bool reg = ps.attached && ps.registered;
            // What else is on the bus (PanelBusWatch) explains a panel that
            // never registers: an OEM radio has it (and we stay silent), or
            // there is no panel at all.
            const bool foreign = _panelWatch && _panelWatch->foreignMaster(millis());
            const bool cluster = !reg && _panelWatch && _panelWatch->clusterOnly();
            // FuncsReg off: the radio owns the handshake and we draw over it,
            // so its sync is the expected bus, not a fault.
            const bool overlay = foreign && _cfg && !_cfg->s().funcReg;
            const uint8_t i = !ps.attached ? 0 : overlay ? 5 : foreign ? 3 : reg ? 2 : cluster ? 4 : 1;
            // A foreign master with FuncsReg on is "broken", not "idle": nothing
            // is drawn, and it may be a false positive worth a look.
            JsonObject o = chain("panel", i == 0 ? "off" : (i == 2 || i == 5) ? "ok" : "broken", W[i]);
            if (_panelWatch) {
                o["foreignMasters"] = _panelWatch->masters();
                o["clusterPings"]   = _panelWatch->clusterPings();
            }
            o["rxFrames"] = _canRx ? *_canRx : 0;
            o["rxPin"]    = (int)Board::PIN_CAN_RX;
            o["txPin"]    = (int)Board::PIN_CAN_TX;
            // How often the link has dropped, and how long ago.
            if (_ui.status) o["resyncs"] = us.resyncs;
            if (us.lastResyncMs)
                o["sinceResyncMs"] = (uint32_t)(millis() - us.lastResyncMs);
            // Shut while nothing on the bus ACKs us (the panel task's TX gate).
            o["txGated"] = ps.txGated;
        }

        // 2. Phone metadata onto the glass; `why` names the first missing precondition.
        {
            const bool on    = haveMs && ms.mediaAuto;
            const bool reg   = ps.attached && ps.registered;
            const bool haveT = haveMedia && box->title()[0];
            static constexpr const char* W[] = {
                "media_off",
                "media_no_panel",
                "media_no_phone",
                "media_no_track",
                "media_ok"};
            const uint8_t i = !on ? 0 : !reg ? 1 : !phone ? 2 : !haveT ? 3 : 4;
            JsonObject o = chain("media", !on ? "off" : i < 4 ? "idle" : "ok", W[i]);
            if (haveMedia) {
                o["title"]  = box->title();
                o["artist"] = box->artist();
                o["clock"]  = box->clock();
            }
            o["delivered"]  = ps.delivered;
            o["superseded"] = ps.superseded;
            o["failed"]     = ps.failed;
        }

        // 3. The clock. Separate from media: the panel draws its clock even with
        //    the display off.
        {
            //    Sources: the phone and the head unit's 0x9D (ClockHealth.h).
            const App::Ui::ClockHealth h =
                App::Ui::clockHealth(haveMs && ms.autoTime, phone, us.clockSrc, millis() - us.clockSetMs);
            JsonObject o = chain("clock", h.state, h.code);
            o["src"] = App::Ui::clockSrcName(us.clockSrc);
            if (haveMedia) o["hhmm"] = box->clock();
        }
#endif

        // 4. Keys, one entry per destination.
        {
            const bool any = _keyEvents && *_keyEvents;
            JsonObject o = chain("keys", any ? "ok" : "idle",
                                 any ? "keys_ok" : "keys_none");
            if (_keyEvents) o["events"] = *_keyEvents;
            App::KeysTask::Routing k;
            if (_st.routing && _st.routing->peek(k)) putDests(o["dest"].to<JsonObject>(), k);
        }

        // 5. Notifications.
        {
            const uint32_t n = _ancsCount ? *_ancsCount : 0;
#if FEATURE_BLE && FEATURE_ANCS
            static constexpr const char* W[] = {
                "ancs_ok",
                "ancs_no_phone",
                "ancs_waiting"};
            JsonObject o = chain("ancs", n ? "ok" : "idle", W[n ? 0 : !phone ? 1 : 2]);
            o["count"] = n;
            o["popup"] = _cfg->s().ancsPopup;
#else
            chain("ancs", "off", "ancs_off");
#endif
        }

        // 6. The head unit on the UART: alive while a frame came within
        //    HuMediaSource's link timeout. Builds without the panel have no
        //    HuMediaSource: hu_seen, a frame since boot, recency unknown.
        {
            static constexpr const char* W[] = {
                "hu_off",
                "hu_silent",
                "hu_quiet",
                "hu_ok",
                "hu_seen",
                "hu_dialect_vw"};
            const uint32_t n = _huRx ? _huRx->frames() : 0;
#if FEATURE_DISPLAY_CAN
            const uint8_t live = !_hu ? 4 : _hu->heard(millis()) ? 3 : 2;
#else
            const uint8_t live = 4;
#endif
            // Wired and talking, but in the other profile: nothing it sends is
            // understood and nothing we send is shown. Set its canbox profile
            // to Toyota RAV4 (the head unit's car settings).
            const bool vw = _huDialect && _huDialect->kind() == Canbox::HuDialect::Kind::Vw;
            const uint8_t i = !_cfg->s().uartEnabled ? 0 : !n ? 1 : vw ? 5 : live;
            JsonObject o = chain("hu", i == 0 ? "off" : i == 3 ? "ok" : i == 5 ? "broken" : "idle", W[i]);
            o["rxFrames"] = n;
            if (_huDialect) {
                o["dialect"] = Canbox::HuDialect::name(_huDialect->kind());
                // 0xF3 NACKs: frames of ours the unit says it does not support.
                o["notSupported"] = _huDialect->notSupported();
            }
            // 0x28 frames that changed: each may pop the climate overlay
            // (Rav4Emitter::climateChanges, TempHold.h). A counter, read as is.
            if (_emitter) o["climateChanges"] = _emitter->climateChanges();
            // Wiring check: nothing at all = wrong pin or no ground; junk and
            // no frames = wired, wrong speed or dialect; bad checksums = noise.
            o["junk"]   = _huRx ? _huRx->junkBytes() : 0;
            o["badCs"]  = _huRx ? _huRx->badChecksums() : 0;
        }

#if FEATURE_KNOBS
        // 6b. The knobs and their rings, for plugging them in one at a time.
        if (_knobs) {
            const Knobs::Service::Live v = _knobs->live();
            const Knobs::Config c = _knobs->config();
            const bool found = !Board::KNOBS_I2C || v.expAddr;
            JsonObject o = chain("knobs", found ? "ok" : "broken",
                                 found ? "knobs_ok"
                                       : "knobs_no_expander");
            o["addr"]      = v.expAddr;
            o["i2cErrors"] = v.expErrors;
            o["right"]     = v.pos[0];
            o["left"]      = v.pos[1];
            o["clicks"]    = v.clicks + v.clicks2;
            o["lineErrors"] = v.encErrors + v.enc2Errors;

            JsonObject l = chain("lights", !c.ledEnabled ? "off" : v.stripOk ? "ok" : "broken",
                                 !c.ledEnabled ? "lights_off"
                                 : v.stripOk   ? "lights_ok"
                                               : "lights_fail");
            l["leds"]   = v.leds;
            l["frames"] = v.frames;
        }
#endif

        // 7. The phone link itself, last, because everything above cites it.
        chain("phone", phone ? "ok" : "idle",
              phone ? "phone_ok"
              : _cfg->s().bleMode == "off" ? "phone_ble_off"
                                           : "phone_advertising");
        d["bleMode"] = _cfg->s().bleMode;

        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

#if FEATURE_BLE && FEATURE_ANCS
    // Cached ANCS notifications, raw UTF-8 (the panel gets the ASCII-folded copy).
    route("/api/notifications", HTTP_GET, [this](PsychicRequest* r) {
        JsonDocument d;
        d["popup"] = _cfg->s().ancsPopup;
        JsonArray a = d["items"].to<JsonArray>();
        // Newest first; capped because the reply is built in RAM.
        auto recent = AppleNotificationService::GetRecent();
        uint8_t n = 0;
        for (const auto& it : recent) {
            if (n++ >= 20) break;
            JsonObject o = a.add<JsonObject>();
            o["uid"]      = it.uid;
            o["category"] = AppleNotificationService::CategoryName(it.categoryId);
            o["app"]      = AppleNotificationService::AppName(it.appId);
            o["appId"]    = it.appId;
            o["title"]    = it.title;
            o["message"]  = it.message;
        }
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });
#endif

    // ---- key routing ---------------------------------------------------------
    // Where a press goes, per destination, as a class mask: the registry's
    // route* entries (GET/POST /api/settings, staged for the keys task);
    // each destination's mask and liveness is the keys chain of /api/health.

    // Blocks for a couple of seconds; explicit request only.
    route("/api/wifi/scan", HTTP_POST, [this](PsychicRequest* r) {
        if (_wifi) _wifi->scanBegin();
        const int n = WiFi.scanNetworks();
        JsonDocument d;
        JsonArray a = d["networks"].to<JsonArray>();
        for (int i = 0; i < n && i < 30; ++i) {
            JsonObject o = a.add<JsonObject>();
            o["ssid"]   = WiFi.SSID(i);
            o["rssi"]   = WiFi.RSSI(i);
            o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        }
        WiFi.scanDelete();
        if (_wifi) _wifi->scanEnd();
        d["count"] = n;
        d["current"] = WiFi.isConnected() ? WiFi.SSID() : String();
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // {ssid, pass, ssid2, pass2}, either pair or both. An empty password keeps
    // the stored one while the name stays the same; a new name with no password
    // is an open network. The first network applies at boot, the second live.
    route("/api/wifi", HTTP_POST, [this](PsychicRequest* r) {
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        const bool has1 = d["ssid"].is<const char*>(), has2 = d["ssid2"].is<const char*>();
        const String s1 = d["ssid"] | "", s2 = d["ssid2"] | "";
        String p1 = d["pass"] | "";
        const String p2 = d["pass2"] | "";
        if (!has1 && !has2) return r->reply(400, "text/plain", "param ssid");
        if (s1.length() > 32 || s2.length() > 32)
            return r->reply(400, "text/plain", "param ssid");
        auto bad = [](const String& p) { return p.length() && (p.length() < 8 || p.length() > 64); };
        if (bad(p1) || bad(p2)) return r->reply(400, "text/plain", "pass_len");
        if (has2 && !_wifi) return r->reply(503, "text/plain", "not_built wifi_manager");
        auto& s = _cfg->s();
        // Only the first network needs the reboot: WifiManager takes the second
        // (setNet2) at its next AP retry.
        const bool changed = has1 && (s1 != s.ssid || (p1.length() && p1 != s.pass));
        if (has1) {
            if (p1.isEmpty() && s1 == s.ssid) p1 = s.pass;
            s.ssid = s1;
            s.pass = p1;
            _cfg->save();
        }
        if (has2) _wifi->setNet2(s2, p2);   // the same rule for an empty password
        LogBus::line(changed ? "wifi saved, pending reboot" : "wifi saved");
        return r->reply(200, "application/json", changed ? "{\"ok\":true,\"pendingReboot\":true}"
                                                         : "{\"ok\":true}");
    });

#if FEATURE_DISPLAY_CAN
    routesDisplay();
    routesAffaMap();
    routesMedia();
#endif
#if FEATURE_VEHICLE_CAN
    routesCan();
#endif
#if FEATURE_CAN2
    routesCan2();
#endif
#if FEATURE_CAPTURE
    routesCap();
#endif
#if FEATURE_GH_OTA
    routesGhOta();
#endif
#if FEATURE_LOG_PUSH
    routesLogPush();
#endif
#if FEATURE_RESISTIVE_KEYS
    routesRes();
#endif
#if FEATURE_KNOBS
    routesKnobs();
#endif

    routesCanbox();
    routesMap();
    routesMacros();

    // Wipes NVS including the WiFi credentials: the board returns in AP mode at
    // http://192.168.4.1/ and the password is gone. Requires ?confirm=factory.
    route("/api/factory", HTTP_POST, [this](PsychicRequest* r) {
        if (paramStr(r, "confirm") != "factory")
            return r->reply(400, "application/json",
                            "{\"ok\":false,\"error\":\"confirm factory\"}");
        LogBus::line("[cfg] FACTORY RESET — NVS wiped, rebooting into AP mode");
        _cfg->factoryReset();
        return replyAndRestart(r, "{\"ok\":true,\"note\":\"AP mode after reboot\"}");
    });
}

#if FEATURE_DISPLAY_CAN
// ---- OEM display -------------------------------------------------------------
//
// Every render call here only enqueues; success means queued, not drawn.
void WebUi::routesDisplay()
{
    route("/api/display", HTTP_GET, [this](PsychicRequest* r) {
        JsonDocument doc;
        fillDisplay(doc.to<JsonObject>());
        String out; serializeJson(doc, out);
        return replyJson(r, out);
    });

    // Display type and registration role are read once at boot: saved, and
    // pendingReboot when either changed.
    route("/api/display", HTTP_POST, [this](PsychicRequest* r) {
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        auto& s = _cfg->s();
        const String type = d["type"] | s.displayType.c_str();
        if (!Catalog::panelKnown(type.c_str())) return r->reply(400, "text/plain", "param type");
        const bool funcReg = d["funcReg"] | s.funcReg;
        const bool changed = type != s.displayType || funcReg != s.funcReg;
        s.displayType = type;
        s.funcReg     = funcReg;
        _cfg->save();
        return r->reply(200, "application/json", changed ? "{\"ok\":true,\"pendingReboot\":true}"
                                                         : "{\"ok\":true}");
    });

    // One reply for every render route: 202 when the library took it (the
    // panel's answer comes later; /api/display reports lastResult), 503 when
    // it refused (PanelOps logs why).
    auto posted = [this](PsychicRequest* r, bool ok) {
        JsonDocument d;
        d["queued"] = ok;
        d["note"] = ok ? "queued for the display task; the result is reported in the log"
                       : "refused — nothing was drawn; the reason is in the log";
        String out; serializeJson(d, out);
        return r->reply(ok ? 202 : 503, "application/json", out.c_str());
    };

    route("/api/display/text", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        if (!r->hasParam("text")) return r->reply(400, "text/plain", "param text");
        const String t = r->getParam("text")->value();
        LogBus::printf("display text: %s", t.c_str());
        return posted(r, _ops->postText(t.c_str()));
    });

    route("/api/display/clock", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        if (!r->hasParam("time")) return r->reply(400, "text/plain", "param time");
        const String t = r->getParam("time")->value();
        const bool ok = _ops->postTime(t.c_str());
        // The ui task owns the clock's record (GET /api/media clockSrc): tell
        // it when the time is a real HHMM. It does not draw it again.
        const int hh = t.substring(0, 2).toInt(), mm = t.substring(2, 4).toInt();
        if (ok && _ui.manualClock && t.length() == 4 && isDigit(t[0]) && isDigit(t[1]) &&
            isDigit(t[2]) && isDigit(t[3]) && hh < 24 && mm < 60)
            _ui.manualClock((uint16_t)(hh << 8 | mm));
        return posted(r, ok);
    });

    route("/api/display/power", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        return posted(r, _ops->postPower(paramBool(r, "on", true)));
    });

    // Hand the glass back to automatic repaints now. `ms` sets the hold for later
    // draws; 0 = hold until this route is called.
    route("/api/display/release", HTTP_POST, [this](PsychicRequest* r) {
        if (!_ui.releaseGlass) return r->reply(503, "text/plain", "not_built display");
        if (r->hasParam("ms") && _ui.glassHoldMs)
            *_ui.glassHoldMs = paramU32(r, "ms", "10000", 10);
        _ui.releaseGlass();
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // Three rows of arbitrary ASCII: the general "draw anything" surface.
    route("/api/display/menu", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const String h  = paramStr(r, "header");
        const String r0 = paramStr(r, "row0");
        const String r1 = paramStr(r, "row1");
        uint8_t scroll = 0x0B;   // carminat::kScrollDown, the library's default
        if (r->hasParam("scroll")) {
            const uint32_t v = paramU32(r, "scroll", "11", 0);
            if (!Catalog::arrowsKnown((int)v)) return r->reply(400, "text/plain", kScrollWhy);
            scroll = (uint8_t)v;
        }
        return posted(r, _ops->postMenu(h.c_str(), r0.c_str(), r1.c_str(), scroll));
    });

    // Move the selection bar. Own RenderSlot, so it and a pending redraw never
    // replace each other.
    route("/api/display/highlight", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        return posted(r, _ops->postHighlight((uint8_t)atoi(paramStr(r, "row", "0").c_str())));
    });

    // icon / src / fmt exposed because they are still being reverse-engineered.
    route("/api/display/popup", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        if (paramBool(r, "hide")) return posted(r, _ops->postHidePopup());
        if (!r->hasParam("text")) return r->reply(400, "text/plain", "param text");
        const uint8_t icon = (uint8_t)paramU32(r, "icon", "0x55", 0);
        const uint8_t src  = (uint8_t)paramU32(r, "src",  "0xFF", 0);
        const uint8_t fmt  = (uint8_t)paramU32(r, "fmt",  "0x60", 0);
        const bool ok = _ops->postPopup(paramStr(r, "text").c_str(), icon, src, fmt);
        // `ms` arms the auto-hide (default 3 s, as the firmware's own popups);
        // ms=0 leaves it up until hide=1 or /api/display/clear.
        if (ok && _armPopup) {
            const uint32_t ms = paramU32(r, "ms", "3000", 0);
            if (ms) _armPopup(ms);
        }
        return posted(r, ok);
    });

    // The same path as a phone notification: fullscreen, sender + wrapped body.
    route("/api/display/notify", HTTP_POST, [this](PsychicRequest* r) {
        if (!_notify) return r->reply(503, "text/plain", "not_built display");
        _notify(paramStr(r, "title").c_str(), paramStr(r, "body").c_str());
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // Back to the media screen: ends a splash/notification, hides a popup,
    // releases the glass hold, resends text and pane.
    route("/api/display/clear", HTTP_POST, [this](PsychicRequest* r) {
        if (!_clear) return r->reply(503, "text/plain", "not_built display");
        _clear();
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    route("/api/display/confirm", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const String cap = paramStr(r, "caption");
        const String r0  = paramStr(r, "row0");
        const String r1  = paramStr(r, "row1");
        return posted(r, _ops->postConfirmBox(cap.c_str(), r0.c_str(), r1.c_str()));
    });

    // The mode 0x05 message box with a real button count (0..2; confirm is fixed
    // at one). `labels` is comma-separated, each truncated to 6 bytes. The
    // two-button form is 119 wire bytes, the longest message we send, and wraps
    // the ISO-TP counter to 0x20.
    route("/api/display/msgbox", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const String r0  = paramStr(r, "row0");
        const String r1  = paramStr(r, "row1");
        const String lab = paramStr(r, "labels");
        const uint8_t n  = (uint8_t)paramU32(r, "buttons", "1", 10);
        const uint8_t se = (uint8_t)paramU32(r, "selected", "0", 10);
        return posted(r, _ops->postMessageBox(r0.c_str(), r1.c_str(), lab.c_str(), n, se));
    });

    // `03 29 05 <n>`: select a message-box button. Not /api/display/highlight,
    // which is the list's `29 01 <rowtag>`.
    route("/api/display/selectbutton", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const uint8_t i = (uint8_t)paramU32(r, "index", "0", 10);
        return posted(r, _ops->postSelectButton(i));
    });

    // setText with the four header bytes exposed, including iconBank2. Icon
    // polarity is inverted (set bit = icon off, so "no icons" is 0x55). Defaults
    // are the library's captured values, so a bare call is a plain setText.
    route("/api/display/styled", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const String t = paramStr(r, "text");
        auto byteOf = [&](const char* k, uint32_t dflt) -> uint8_t {
            if (!r->hasParam(k)) return (uint8_t)dflt;
            return (uint8_t)paramU32(r, k, "0", 0);
        };
        return posted(r, _ops->postTextStyled(t.c_str(),
                                              byteOf("icon", 0x55),
                                              byteOf("src", 0xFF),
                                              byteOf("fmt", 0x60),
                                              byteOf("bank2", 0x55)));
    });

    // The three-row info list: three messages on the wire, one per row, not
    // coalesced. No hide command is known; draw the next screen instead.
    route("/api/display/info", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const String l1 = paramStr(r, "l1");
        const String l2 = paramStr(r, "l2");
        const String l3 = paramStr(r, "l3");
        return posted(r, _ops->postInfoPopup(l1.c_str(), l2.c_str(), l3.c_str()));
    });

    // No hide: a fullscreen is not an overlay, setText() over it replaces it
    // (measured on the panel). `02 54 03` is popup?hide=1.
    route("/api/display/fullscreen", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const String l1 = paramStr(r, "l1");
        const String l2 = paramStr(r, "l2");
        const String l3 = paramStr(r, "l3");
        return posted(r, _ops->postFullscreen(l1.c_str(), l2.c_str(), l3.c_str()));
    });

    // Inject a key as if the panel sent it (KeySource::Local, see PanelOps);
    // same path as a key decoded off the wire.
    //
    // wire=1 instead puts the key frame on CAN B as the panel would
    // (KeySource::Wire): for driving a FACTORY radio that shares the bus with
    // us, to test it. In the usual radio role it means nothing (nobody but us
    // listens for panel keys) and the frame carries the panel's own id, so with
    // the real panel attached it is a phantom press other modules may act on.
    // The library sends it only while the link is up and the opening is done
    // (or passive); without that it would only be refused in the log, so 409.
    route("/api/display/key", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        if (!r->hasParam("code")) return r->reply(400, "text/plain", "param code");
        const uint16_t code = (uint16_t)paramU32(r, "code", "0", 0);
        const bool wire = paramBool(r, "wire"), hold = paramBool(r, "hold");
        if (wire && hold && !Keys::AffaMap::wireHold((affa::Key)code))
            return r->reply(400, "text/plain", "held_no_wire");
        if (wire) {
            const MegaOpen::PanelStatus st = _panel ? _panel->read() : MegaOpen::PanelStatus{};
            if (!st.synced && !st.passive)
                return r->reply(409, "text/plain", "panel_down");
        }
        return posted(r, _ops->postKey(code, hold, wire));
    });

    // Re-run the handshake: begin() clears both FSMs and cancels the queue. The
    // library posts it to the panel task, in order with the renders posted
    // before it (they complete Cancelled).
    route("/api/display/resync", HTTP_POST, [this](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        if (!_ops->postResync()) return r->reply(503, "text/plain", "queue_full");
        LogBus::line("resync requested");
        return r->reply(200, "application/json",
                        "{\"ok\":true,\"note\":\"handshake restarting; watch registered\"}");
    });

    // Drop everything queued but not started; the transfer in flight completes.
    route("/api/display/abort", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        return posted(r, _ops->postAbort());
    });
}

// ---- the stalk mapping --------------------------------------------------------
//
// Sixteen rows: eight keys x {click, hold}. Each has an action (seen by every
// sink) and an optional literal RAV4 code (0 = profile default for the action).
void WebUi::routesAffaMap()
{
    // The map the keys task looks keys up in is staged: the page reads and
    // writes the newest edit, and the keys task swaps it in at its next pass.
    route("/api/affamap", HTTP_GET, [this](PsychicRequest* r) {
        if (!_affaMap || !_st.affa) return r->reply(503, "text/plain", "not_built display");
        char last[160];     // worst case ~130: 10-digit seq, longest key and action, all five dests
        keyLastJson(last, sizeof(last));

        // The wiring check polls this every 300 ms: one fixed buffer, no heap.
        if (paramBool(r, "live")) {
            char out[288];
            const uint16_t u = _affaSrc ? _affaSrc->lastUnmapped() : 0;
            snprintf(out, sizeof(out),
                     "{\"last\":%s,\"unmapped\":{\"count\":%u,\"last\":%u,\"hold\":%s,\"known\":%s},"
                     "\"events\":%u}",
                     last, (unsigned)(_affaSrc ? _affaSrc->unmapped() : 0), (unsigned)u,
                     _affaSrc && _affaSrc->lastUnmappedHold() ? "true" : "false",
                     Keys::AffaMap::indexOf((affa::Key)u) < Keys::AffaMap::KEYS ? "true" : "false",
                     (unsigned)(_affaSrc ? _affaSrc->events() : 0));
            return r->reply(200, "application/json", out);
        }

        Keys::AffaMap map;
        if (!_st.affa->peek(map)) return r->reply(503, "text/plain", "busy");
        JsonDocument d;

        JsonArray rows = d["rows"].to<JsonArray>();
        for (uint8_t k = 0; k < Keys::AffaMap::KEYS; ++k) {
            for (uint8_t h = 0; h < 2; ++h) {
                const uint8_t i = Keys::AffaMap::rowIndex(k, h != 0);
                const Keys::AffaMap::Row& row = map.row(i);
                JsonObject o = rows.add<JsonObject>();
                o["index"]  = i;
                o["key"]    = Keys::AffaMap::keyName(k);
                o["wire"]   = (uint16_t)Keys::AffaMap::keyAt(k);
                o["hold"]   = (h != 0);
                o["action"] = Keys::name(row.action);
                o["code"]   = row.code;
                o["asLong"] = row.asLong;
                // What actually goes on the wire: override, else profile default.
                o["effective"] = row.code ? row.code : Canbox::rav4CodeFor(row.action);
                o["effectiveName"] =
                    Canbox::keyCodeName(row.code ? row.code : Canbox::rav4CodeFor(row.action));
            }
        }

        if (_affaSrc) {
            JsonObject u = d["unmapped"].to<JsonObject>();
            u["count"] = _affaSrc->unmapped();
            u["last"]  = _affaSrc->lastUnmapped();
            u["hold"]  = _affaSrc->lastUnmappedHold();
            // known=false: a code outside the eight keys (new stalk); true: an emptied row.
            u["known"] = Keys::AffaMap::indexOf((affa::Key)_affaSrc->lastUnmapped())
                             < Keys::AffaMap::KEYS;
            d["events"] = _affaSrc->events();
        }
        d["last"] = serialized((const char*)last);
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    route("/api/affamap", HTTP_POST, [this](PsychicRequest* r) {
        if (!_affaMap || !_st.affa) return r->reply(503, "text/plain", "not_built display");
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");

        uint16_t n = 0;
        Keys::AffaMap saved;
        const bool ok = _st.affa->edit([&](Keys::AffaMap& m) {
            for (JsonObjectConst o : d["rows"].as<JsonArrayConst>()) {
                const uint8_t i = o["index"] | 0xFF;
                if (i >= Keys::AffaMap::ROWS) continue;
                Keys::AffaMap::Row row;
                row.action = Keys::fromName(o["action"] | "None");
                row.code   = (uint8_t)(o["code"] | 0);
                row.asLong = o["asLong"] | false;
                m.setRow(i, row);
                ++n;
            }
            saved = m;
        });
        if (!ok) return r->reply(503, "text/plain", "busy");
        App::KeyBus::kick();
        _cfg->saveAffaMap(saved);
        LogBus::printf("stalk mapping saved (%u rows)", (unsigned)n);
        JsonDocument res; res["ok"] = true; res["applied"] = n;
        String out; serializeJson(res, out);
        return replyJson(r, out);
    });

    route("/api/affamap/defaults", HTTP_POST, [this](PsychicRequest* r) {
        if (!_affaMap || !_st.affa) return r->reply(503, "text/plain", "not_built display");
        Keys::AffaMap saved;
        if (!_st.affa->edit([&](Keys::AffaMap& m) { m.loadDefaults(); saved = m; }))
            return r->reply(503, "text/plain", "busy");
        App::KeyBus::kick();
        _cfg->saveAffaMap(saved);
        LogBus::line("stalk mapping reset to defaults");
        return r->reply(200, "application/json", "{\"ok\":true}");
    });
}

// {seq,key,wire,hold,action,dests[]}: the last panel key as the keys task
// resolved it, or {seq:0} before the first. A copy of the snapshot only.
void WebUi::keyLastJson(char* out, size_t cap) const
{
    Keys::PanelKeyLast k;
    const uint32_t seq = _keyLast ? _keyLast->read(k) : 0;
    if (!seq) { snprintf(out, cap, "{\"seq\":0}"); return; }
    char dests[48] = "";
    size_t n = 0;
    for (uint8_t i = 0; i < Keys::DEST_COUNT && n < sizeof(dests); ++i)
        if (k.dests & (1u << i))
            n += snprintf(dests + n, sizeof(dests) - n, "%s\"%s\"", n ? "," : "", Keys::DEST_IDS[i]);
    snprintf(out, cap,
             "{\"seq\":%u,\"key\":\"%s\",\"wire\":%u,\"hold\":%s,\"action\":\"%s\",\"dests\":[%s]}",
             (unsigned)seq, k.key < Keys::AffaMap::KEYS ? Keys::AffaMap::keyName(k.key) : "",
             (unsigned)k.wire, k.hold ? "true" : "false",
             Keys::name((Keys::Action)k.action), dests);
}

// ---- the now-playing screen ---------------------------------------------------
// The newest settings, applied or not: what the page should show.
bool WebUi::mediaSettings(App::Ui::MediaSettings& out) const
{
    return _ui.settings && _ui.settings->peek(out);
}

void WebUi::routesMedia()
{
    route("/api/media", HTTP_GET, [this](PsychicRequest* r) {
        if (!_ui.media || !_ui.settings) return r->reply(503, "text/plain", "not_built display");
        // Copies only: the ui task owns the screen and the pane. Not on the
        // stack (~320 + ~1200 bytes on a 4 kB httpd stack) and not static
        // (internal RAM): for this request only (Scratch).
        Scratch<MegaOpen::MediaScreen> box;
        if (!box) return r->reply(503, "text/plain", "no_memory");
        MegaOpen::MediaScreen& media = *box;
        if (!_ui.media->copy(media)) return r->reply(503, "text/plain", "busy");
        App::Ui::MediaSettings ms;
        if (!mediaSettings(ms)) return r->reply(503, "text/plain", "busy");
        const App::Ui::UiStatus us = _ui.status ? _ui.status->read() : App::Ui::UiStatus{};

        JsonDocument d;
        d["auto"]     = ms.mediaAuto;
        d["title"]    = media.title();
        d["artist"]   = media.artist();
        // Empty until the phone has reported a time.
        d["clock"]    = media.clock();
        // Who set the panel's clock last (hu, cts, manual), and to what.
        d["clockSrc"] = App::Ui::clockSrcName(us.clockSrc);
        char at[6] = "";
        if (us.clockSrc != App::Ui::ClockSrc::None)
            snprintf(at, sizeof(at), "%02u:%02u", (unsigned)(us.clockAt >> 8), (unsigned)(us.clockAt & 0xFF));
        d["clockAt"]  = at;
        d["state"]    = (uint8_t)media.state();
        d["elapsed"]  = media.elapsed();
        d["duration"] = media.duration();
        d["cols"]     = ms.cols;
        d["bar"]      = ms.bar;
        // `scrollMs` is the title row, kept for existing readers.
        d["scrollMs"]       = ms.scrollMsTitle;
        d["scrollMsTitle"]  = ms.scrollMsTitle;
        d["scrollMsArtist"] = ms.scrollMsArtist;
        d["layout"]   = MegaOpen::MediaScreen::layoutName(ms.layout);
        d["splashMs"] = ms.splashMs;

        // The preview below is composed on the copy with the newest settings,
        // by the same code that drives the panel, so the page sees its own edit.
        media.setLayout(ms.layout);
        media.setCols(ms.cols);
        media.setBarWidth(ms.bar);
        media.setScrollMsTitle(ms.scrollMsTitle);
        media.setScrollMsArtist(ms.scrollMsArtist);
        media.setStyle(ms.style);
        d["style"] = MegaOpen::MediaScreen::styleName(ms.style);
        d["scroll"] = ms.scroll;
        // False when the head unit is the source: it reports no state or position.
        d["hasTransport"] = media.hasTransport();

        // ---- which source, and what the head unit has told us ---------------
        if (_router) {
            d["source"]       = MediaRouter::modeStr(_router->mode());
            d["sourceActive"] = _router->sourceName();   // what `auto` actually picked
            d["sourceLive"]   = _router->active();
            d["sourceStatus"] = _router->statusText();
            // What the source sent, UTF-8, before the panel's ASCII fold:
            // `title`/`artist` above are the glass's copy (Cyrillic transliterated).
            const MediaInfo sm = _router->snapshot();
            d["srcTitle"]  = sm.title;
            d["srcArtist"] = sm.artist;
            d["srcAlbum"]  = sm.album;
        }
        if (_hu) {
            // huFrames climbing with huTextFrames at 0: the unit talks but sends
            // no text. Both 0: dead UART. huLearned* is what the learner chose,
            // hu*Cmd is what was pinned.
            d["huFrames"]        = _hu->frames();
            d["huTextFrames"]    = _hu->textFrames();
            d["huLearnedTitle"]  = _hu->learnedTitleCmd();
            d["huLearnedArtist"] = _hu->learnedArtistCmd();
            d["huTitleCmd"]      = _hu->titleCmd();
            d["huArtistCmd"]     = _hu->artistCmd();
            d["huTitleFirst"]    = _hu->titleFirst();
            // A copy under the source's mutex: the hu task writes these strings.
            const MediaInfo hm = _hu->snapshot();
            d["huTitle"]         = hm.title;
            d["huArtist"]        = hm.artist;
            d["huAlbum"]         = hm.album;
        }

        // ---- was it drawn, or only queued -----------------------------------
        // `busy`: wire mid-transfer, so a repaint was skipped. `superseded`
        // tracking `delivered` means repaints outrun the panel.
        if (_panel) {
            const MegaOpen::PanelStatus ps = _panel->read();
            d["txBusy"]       = ps.busy;
            d["txDelivered"]  = ps.delivered;
            d["txSuperseded"] = ps.superseded;
            d["txFailed"]     = ps.failed;
            d["txLast"]       = affa::resultName((affa::Result)ps.lastDelivery);
        }

        // Where the metadata comes from ("no title" and "no phone" look the same).
        d["phone"]    = _phoneConnected ? _phoneConnected() : false;
        d["autoTime"] = ms.autoTime;
        d["bleMode"]  = _cfg ? _cfg->s().bleMode : String("off");
        if (_router) {
            // snapshot(), not current(): AMS writes MediaInfo from NimBLE's host
            // task; the copy is taken under the source's mutex.
            const MediaInfo m = _router->snapshot();
            d["srcTitle"]  = m.title;
            d["srcArtist"] = m.artist;
            d["srcAlbum"]  = m.album;
            d["srcPlayer"] = m.playerName;
            d["srcState"]  = (uint8_t)m.playbackState;
            d["srcDur"]    = (uint16_t)m.duration;
            d["srcElapsed"] = (uint16_t)m.elapsedAt(millis());
        }

        // The rows exactly as they would be sent now.
        char r1[MegaOpen::MediaScreen::ROW_MAX];
        char r2[MegaOpen::MediaScreen::ROW_MAX];
        char r3[MegaOpen::MediaScreen::ROW_MAX];
        media.compose(millis(), r1, r2, r3);
        JsonArray p = d["preview"].to<JsonArray>();
        p.add(r1); p.add(r2); p.add(r3);

        // ---- the graphic layer ------------------------------------------
        // The pane preview, composed on the board by the panel's code. 288 bytes
        // as 576 hex characters (readable, unlike base64).
        // Its ~1.2 kB of working space for this request only (Scratch), in
        // this scope because pn["bits"] points at pw->hex until serializeJson.
        // No memory: the reply goes out without the pane.
        struct PaneWork {
            MegaOpen::MediaPane snap;
            uint8_t bits[MegaOpen::MediaPane::BYTES];
            char    hex[MegaOpen::MediaPane::BYTES * 2 + 1];
        };
        Scratch<PaneWork> pw;
        if (pw && _ui.pane && _ui.pane->copy(pw->snap)) {
            JsonObject pn = d["pane"].to<JsonObject>();
            pn["scene"]     = MegaOpen::MediaPane::sceneName(pw->snap.scene());
            pn["on"]        = pw->snap.on();
            pn["animates"]  = MegaOpen::MediaPane::animates(pw->snap.scene());
            pn["usesMedia"] = MegaOpen::MediaPane::usesMedia(pw->snap.scene());
            pn["sent"]      = us.panesSent;
            pn["failed"]    = us.panesFailed;
            // Animation steps per delivered image (the image rate is the wire's).
            pn["speed"]     = ms.paneSpeed;
            // Carousel: `scene` is the live stop, `auto` whether it advances.
            pn["auto"]      = ms.paneAuto;
            pn["cycleS"]    = ms.paneCycleS;

            // Only setText and showInfoMenu leave the pane's half of the glass
            // alone; `menu` and `fullscreen` cover it while it is still delivered.
            pn["visible"] = MegaOpen::MediaScreen::sharesWithPane(ms.layout);
            if (pw->snap.on() && !MegaOpen::MediaScreen::sharesWithPane(ms.layout))
                pn["hiddenBy"] = MegaOpen::MediaScreen::layoutName(ms.layout);

            // Composing doesn't advance the animation; the copy is ours anyway.
            // Except Custom: the copy's picture pointer is the ui task's live
            // buffer (MediaPane::setCustom), which httpd must not read. The
            // picture comes from the staged copy, under its mutex, instead.
            // This is the only place httpd composes a pane.
            if (pw->snap.scene() == MegaOpen::MediaPane::Scene::Custom) {
                memset(pw->bits, 0, sizeof(pw->bits));
                if (_ui.paneBits)
                    _ui.paneBits->read([&](const App::Ui::PaneBits& p) { memcpy(pw->bits, p.bits, sizeof(pw->bits)); });
            } else {
                pw->snap.compose(pw->bits);
            }
            MegaOpen::MediaPane::toHex(pw->bits, pw->hex);
            pn["bits"] = (const char*)pw->hex;
        }

        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

#if FEATURE_BLE
    // BLE state. `bonded` false while advertising: pairing never completed on the
    // phone. `bonded` true, `connected` false: stale bond, forget on both sides.
    route("/api/ble", HTTP_GET, [this](PsychicRequest* r) {
        if (!_bleStatus) return r->reply(503, "text/plain", "not_built ble");
        const String s = _bleStatus();
        return replyJson(r, s);
    });

    // Forgets every bond (no per-peer delete). Clears a half-pair only if the
    // phone also does "Forget This Device".
    route("/api/ble/unbond", HTTP_POST, [this](PsychicRequest* r) {
        if (!_bleUnbond) return r->reply(503, "text/plain", "not_built ble");
        _bleUnbond();
        LogBus::line("BLE: all bonds cleared — now Forget This Device on the phone too");
        return r->reply(200, "application/json",
                        "{\"ok\":true,\"note\":\"forget it on the phone as well\"}");
    });
#endif

    route("/api/media", HTTP_POST, [this](PsychicRequest* r) {
        if (!_ui.settings) return r->reply(503, "text/plain", "not_built display");
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        using MegaOpen::MediaPane;
        if (d["scroll"].is<int>() && !Catalog::arrowsKnown(d["scroll"].as<int>()))
            return r->reply(400, "text/plain", kScrollWhy);

        // `bits`: the Custom scene's picture (MediaPane.h format, 576 hex).
        // Checked whole, decoded straight into the staging buffer, stored, and
        // then the scene chosen.
        const bool hasBits = !d["bits"].isNull();
        if (hasBits) {
            const char* hex = d["bits"].as<const char*>();
            if (!MediaPane::fromHex(hex, nullptr))
                return r->reply(400, "text/plain", "param bits");
            if (!_ui.paneBits || !_ui.paneBits->edit([&](App::Ui::PaneBits& p) {
                    MediaPane::fromHex(hex, p.bits);
                    p.set = true;
                }))
                return r->reply(503, "text/plain", "busy");
            // The NVS write runs under the staging mutex (no second buffer).
            // The ui task only try-takes it (Staged::applyIfDirty), so it never
            // waits: the new picture lands on its first pass after this.
            bool saved = false;
            _ui.paneBits->read([&](const App::Ui::PaneBits& p) { saved = _cfg->savePaneBits(p.bits, sizeof(p.bits)); });
            if (!saved) return r->reply(500, "text/plain", "nvs_failed");
        }
        const char* paneName = hasBits ? "custom" : d["pane"].as<const char*>();
        if (paneName && !strcmp(paneName, "custom") && !hasBits) {
            bool drawn = false;
            if (_ui.paneBits) _ui.paneBits->read([&](const App::Ui::PaneBits& p) { drawn = p.set; });
            if (!drawn) return r->reply(409, "text/plain", "no_custom_pic");
        }

        // Every change is staged; the ui task applies the whole set on its next
        // pass (Staged<T>, §3.1). Nothing here touches the live screen or pane.
        bool save = false;
        bool splashNow = false;
        const bool ok = _ui.settings->edit([&](App::Ui::MediaSettings& m) {
            // The test fields: one poke, held until the source's next update.
            App::Ui::MediaPoke& pk = m.poke;
            pk.has = 0;
            if (d["title"].is<const char*>()) {
                strlcpy(pk.title, d["title"], sizeof(pk.title));
                pk.has |= App::Ui::MediaPoke::TITLE;
            }
            if (d["artist"].is<const char*>()) {
                strlcpy(pk.artist, d["artist"], sizeof(pk.artist));
                pk.has |= App::Ui::MediaPoke::ARTIST;
            }
            // The phone overwrites it on the next minute.
            if (d["clock"].is<const char*>()) {
                strlcpy(pk.clock, d["clock"], sizeof(pk.clock));
                pk.has |= App::Ui::MediaPoke::CLOCK;
            }
            if (d["state"].is<int>()) {
                pk.state = (uint8_t)(d["state"] | 0);
                pk.has |= App::Ui::MediaPoke::STATE;
            }
            if (d["elapsed"].is<int>()) {
                pk.elapsed = (uint16_t)(d["elapsed"] | 0);
                pk.has |= App::Ui::MediaPoke::ELAPSED;
            }
            if (d["duration"].is<int>()) {
                pk.duration = (uint16_t)(d["duration"] | 0);
                pk.has |= App::Ui::MediaPoke::DURATION;
            }
            if (pk.has) ++m.pokeSeq;

            if (d["cols"].is<int>()) m.cols = (uint8_t)(d["cols"] | MegaOpen::MediaScreen::COLS_DEFAULT);
            if (d["bar"].is<int>())  m.bar  = (uint8_t)(d["bar"] | 10);
            // `scrollMs` sets both rows, so it goes before the per-row values.
            if (d["scrollMs"].is<int>())
                m.scrollMsTitle = m.scrollMsArtist = (uint16_t)(d["scrollMs"] | 700);
            if (d["scrollMsTitle"].is<int>())  m.scrollMsTitle  = (uint16_t)(d["scrollMsTitle"] | 700);
            if (d["scrollMsArtist"].is<int>()) m.scrollMsArtist = (uint16_t)(d["scrollMsArtist"] | 1400);
            // Scroll arrows, panel encoding (the catalog's `arrows`, validated
            // above): 0x00 none, 0x07 up, 0x0B down, 0x03 both.
            if (d["scroll"].is<int>()) m.scroll = (uint8_t)(d["scroll"] | 0);
            // Persisted. Unknown names must not map to Fullscreen (it hides the pane).
            if (d["layout"].is<const char*>()) {
                m.layout = MegaOpen::MediaScreen::layoutFromName(d["layout"]);
                // Width follows the layout (8 beside the pane, 20 for the menu, 22
                // fullscreen) unless the same request carries `cols`.
                if (!d["cols"].is<int>()) m.cols = MegaOpen::MediaScreen::colsFor(m.layout);
                _cfg->s().mediaLayout = MegaOpen::MediaScreen::layoutName(m.layout);
                save = true;
            }
            // Style: persisted by name; unknown falls back to classic.
            if (d["style"].is<const char*>()) {
                m.style = MegaOpen::MediaScreen::styleFromName(d["style"]);
                _cfg->s().mediaStyle = MegaOpen::MediaScreen::styleName(m.style);
                save = true;
            }
            if (d["auto"].is<bool>()) {
                m.mediaAuto = d["auto"];
                _cfg->s().mediaScreen = m.mediaAuto;
                save = true;
            }
            // Pane scene or "off", persisted by name like the style. "auto"
            // starts the carousel without moving the live scene; any real scene
            // name stops it. sceneSeq makes the ui apply the choice once.
            // `bits` chooses the Custom scene, as `pane:"custom"` would.
            if (paneName) {
                m.paneAuto = !strcmp(paneName, "auto");
                if (m.paneAuto) {
                    _cfg->s().mediaPane = "auto";
                } else {
                    m.paneScene = MegaOpen::MediaPane::sceneFromName(paneName);
                    _cfg->s().mediaPane = MegaOpen::MediaPane::sceneName(m.paneScene);
                }
                ++m.sceneSeq;
                save = true;
            }
            // 5..600 s per stop.
            if (d["paneCycleS"].is<int>()) {
                m.paneCycleS = (uint16_t)constrain(d["paneCycleS"].as<int>(), 5, 600);
                _cfg->s().paneCycleS = m.paneCycleS;
                save = true;
            }
            // 1..6: beyond that it stops reading as motion at two images a second.
            if (d["paneSpeed"].is<int>()) {
                m.paneSpeed = (uint8_t)constrain(d["paneSpeed"].as<int>(), 1, 6);
                _cfg->s().paneSpeed = m.paneSpeed;
                save = true;
            }
            // Track splash: 0 = off, else 1..20 s.
            if (d["splashMs"].is<int>()) {
                const int ms = d["splashMs"].as<int>();
                m.splashMs = (uint16_t)(ms <= 0 ? 0 : constrain(ms, 1000, 20000));
                _cfg->s().trackSplashMs = m.splashMs;
                save = true;
            }
            // Clock push, independent of now-playing.
            if (d["autoTime"].is<bool>()) {
                m.autoTime = d["autoTime"];
                _cfg->s().autoTime = m.autoTime;
                save = true;
            }
            splashNow = d["splashNow"].as<bool>();
        });
        if (!ok) return r->reply(503, "text/plain", "busy");
        // NVS outside the staging mutex: a flash write can take tens of ms.
        if (save) _cfg->save();
        // Fire one now for the current track.
        if (splashNow && _ui.splashNow) _ui.splashNow();

        return r->reply(200, "application/json", "{\"ok\":true}");
    });

}
#endif

#if FEATURE_VEHICLE_CAN
// ---- CAN status + sniffer ------------------------------------------------------
void WebUi::routesCan()
{
    route("/api/can", HTTP_GET, [this](PsychicRequest* r) {
        JsonDocument d;
        // Listen-only on the car's bus; with the panel, the link's TX gate can
        // also have turned transmit off (nobody ACKs, nothing heard), which is
        // why busErr and twaiTx stop moving: say so here.
        bool txEnabled = false;
#if FEATURE_DISPLAY_CAN
        if (_panel) {
            const MegaOpen::PanelStatus p = _panel->read();
            txEnabled = !p.txGated;
            d["txGated"] = p.txGated;
            d["txGates"] = p.txGates;
        }
#endif
        d["txEnabled"] = txEnabled;
        d["listenOnly"] = Diag::CanBench::listenOnly();

        // Controller state: tells "no frames" from "frames but undecodable".
        // The loop's 100 ms sample: httpd does not call the driver.
        const Diag::CanStatus st = Diag::CanBench::status();
        if (st.installed) {
            static const char* const kState[] = {"stopped", "running", "bus-off", "recovering"};
            d["state"]    = kState[st.state <= 3 ? st.state : 0];
            d["txErr"]    = st.tec;
            d["rxErr"]    = st.rec;
            d["txFailed"] = st.txFailed;
            d["rxMissed"] = st.rxMissed;
            d["busErr"]   = st.busErr;
            d["arbLost"]  = st.arbLost;
            d["txQueued"] = st.msgsToTx;
        }
        // Our own transmit tally; the controller counters and the raw ring
        // don't show whether we ever called transmit.
        d["twaiTxOk"]   = st.twaiTxOk;
        d["twaiTxFail"] = st.twaiTxFail;
        d["twaiRx"]     = st.twaiRx;
        d["twaiMode"]   = st.mode == TWAI_MODE_LISTEN_ONLY ? "listen-only" : "normal";
        d["rxFrames"]  = _canRx ? *_canRx : 0;
        if (_sniffer) {
            d["ids"]      = _sniffer->count();
            d["frames"]   = _sniffer->frames();
            d["overflow"] = _sniffer->overflow();
            d["sniff"]    = _sniffer->enabled();
        }
        // The car task (Step 5): who decodes, and how much matched.
        if (App::Car::running()) {
            const App::Car::Stats c = App::Car::stats();
            JsonObject car = d["car"].to<JsonObject>();
            car["sources"]  = c.sources;
            car["frames"]   = c.frames;
            car["fps"]      = c.fps;
            car["decoded"]  = c.decodeHits;
            car["unknown"]  = c.decodeUnknown;
            car["dropped"]  = c.srcDropped;
            car["cmdDrops"] = c.cmdDrops;
        }
        d["rxPin"] = (int)Board::PIN_CAN_RX;
        d["txPin"] = (int)Board::PIN_CAN_TX;
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // Error Code Capture: type (bit/form/stuff/other), direction and frame
    // segment of bus errors. Bit error while sending in the ACK slot = our ACK
    // does not read back; form error in ACK delimiter/EOF while receiving = the
    // other node objects; stuff error in data = timing close but off.
    // Races the driver ISR for the latch, so it is a histogram, not a log.
    route("/api/can/ecc", HTTP_GET, [](PsychicRequest* r) {
        static const char* const kType[4] = {"bit", "form", "stuff", "other"};
        auto segName = [](uint8_t s) -> const char* {
            switch (s) {
                case 0x03: return "SOF";            case 0x02: return "ID28-21";
                case 0x06: return "ID20-18";        case 0x04: return "SRTR";
                case 0x05: return "IDE";            case 0x07: return "ID17-13";
                case 0x0F: return "ID12-5";         case 0x0E: return "ID4-0";
                case 0x0C: return "RTR";            case 0x0D: return "r1";
                case 0x09: return "r0";             case 0x0B: return "DLC";
                case 0x0A: return "data";           case 0x08: return "CRC";
                case 0x18: return "CRC delimiter";  case 0x19: return "ACK slot";
                case 0x1B: return "ACK delimiter";  case 0x1A: return "EOF";
                case 0x12: return "intermission";   case 0x11: return "active error flag";
                case 0x16: return "passive error flag";
                case 0x13: return "tolerate dominant"; case 0x17: return "error delimiter";
                case 0x1C: return "overload flag";  default:   return "?";
            }
        };
        uint16_t hist[256] = {0};
        for (uint16_t i = 0; i < 3000; ++i) {
            ++hist[TWAI.error_code_capture_reg.val & 0xFF];
            delayMicroseconds(40);
            if ((i & 0xFF) == 0xFF) delay(1);        // let the server breathe
        }
        JsonDocument d;
        JsonArray a = d["seen"].to<JsonArray>();
        for (uint16_t v = 0; v < 256; ++v) {
            if (!hist[v]) continue;
            JsonObject o = a.add<JsonObject>();
            o["raw"]   = v;
            o["count"] = hist[v];
            o["type"]  = kType[(v >> 6) & 3];
            o["dir"]   = (v & 0x20) ? "while RECEIVING" : "while SENDING";
            o["where"] = segName(v & 0x1F);
        }
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // Per-id table. `changed`: a bit per byte that differed since the baseline.
    route("/api/can/ids", HTTP_GET, [this](PsychicRequest* r) {
        if (!_sniffer) return r->reply(503, "text/plain", "not_built can");
        JsonDocument d;
        d["overflow"] = _sniffer->overflow();
        JsonArray a = d["ids"].to<JsonArray>();
        // copyAt: the panel task writes these rows while this runs on httpd.
        const uint8_t rows = _sniffer->count();
        for (uint8_t i = 0; i < rows; ++i) {
            Vehicle::CanSniffer::Entry e;
            if (!_sniffer->copyAt(i, e) || !e.used) continue;
            JsonObject o = a.add<JsonObject>();
            o["id"]      = e.id;
            o["count"]   = e.count;
            o["len"]     = e.len;
            o["changed"] = e.changed;
            char hex[24];
            int n = 0;
            for (uint8_t b = 0; b < e.len && b < 8; ++b)
                n += snprintf(hex + n, sizeof(hex) - n, "%02X", e.last[b]);
            o["data"] = hex;
        }
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    route("/api/can/baseline", HTTP_POST, [this](PsychicRequest* r) {
        if (!_sniffer) return r->reply(503, "text/plain", "not_built can");
        // Applied by the panel task, which owns the table, before its next frame.
        _sniffer->requestBaseline();
        LogBus::line("can baseline marked — now do the thing you want to find");
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    route("/api/can/reset", HTTP_POST, [this](PsychicRequest* r) {
        if (!_sniffer) return r->reply(503, "text/plain", "not_built can");
        _sniffer->requestReset();   // the table's owner applies it
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // The per-id table on or off (?on=1|0). Off by default on the T-2CAN, where
    // the recorder is the tool and a busy car bus would pay a table search per
    // frame; on elsewhere. Not saved: every boot starts at the default.
    route("/api/can/sniff", HTTP_POST, [this](PsychicRequest* r) {
        if (!_sniffer) return r->reply(503, "text/plain", "not_built can");
        const bool on = paramStr(r, "on") == "1";
        _sniffer->setEnabled(on);
        LogBus::printf("can sniffer %s", on ? "on" : "off");
        return r->reply(200, "application/json", on ? "{\"ok\":true,\"sniff\":true}"
                                                    : "{\"ok\":true,\"sniff\":false}");
    });
}
#endif

#if FEATURE_CAN2
// ---- CAN A -------------------------------------------------------------------
// The car's bus on the T-2CAN, listen-only always. Everything here is the car
// task's snapshot; a rate set here is handed to that task, which restarts the
// controller on its next wake.
void WebUi::routesCan2()
{
    route("/api/can2", HTTP_GET, [this](PsychicRequest* r) {
        if (!_can2) return r->reply(503, "text/plain", "not_built can2");
        const Mcp2518Source::Status c = _can2->read();
        static const char* const kAb[] = {"searching", "locked", "manual"};
        JsonDocument d;
        d["live"]          = c.live;
        d["listenOnly"]    = true;       // the only mode this firmware has
        d["autobaud"]      = kAb[c.state <= 2 ? c.state : 0];
        d["bitrate"]       = c.bps;      // listening at, now
        d["detected"]      = c.detected; // locked by the search, 0 = not yet
        d["setting"]       = _cfg->s().can2Bitrate;   // 0 = auto
        d["saved"]         = _cfg->s().can2Detected;  // tried first at boot
        d["fps"]           = c.fps;
        d["frames"]        = c.frames;
        d["fdDropped"]     = c.fdDropped;
        d["rtrDropped"]    = c.remoteDropped;
        d["intKicks"]      = c.intKicks;
        d["maskMaxUs"]     = c.maskMaxUs;
        d["hwOverflows"]   = c.hwOverflows;
        d["ringPeak"]      = c.ringPeak;
        d["rec"]           = c.trec & 0xFF;
        d["tec"]           = (c.trec >> 8) & 0xFF;
        d["errFlags"]      = (c.trec >> 16) & 0x3F;
        d["bdiag1"]        = c.bdiag1;
        d["restarts"]      = c.restarts;
        d["rounds"]        = c.cycles;
        d["rejected"]      = c.rejected;
        d["beginErr"]      = c.beginErr;
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // ?bitrate=125000|250000|500000|1000000 fixes the rate; 0 (or auto) goes
    // back to finding it. Saved; the car task applies it at once, no reboot.
    route("/api/can2", HTTP_POST, [this](PsychicRequest* r) {
        if (!_can2Rate) return r->reply(503, "text/plain", "not_built can2");
        const String v = paramStr(r, "bitrate");
        const uint32_t bps = (v == "auto" || !v.length()) ? 0u : (uint32_t)v.toInt();
        if (bps && !Bus::Autobaud::valid(bps))
            return r->reply(400, "text/plain", "param bitrate");
        _cfg->s().can2Bitrate = bps;
        _cfg->save();
        _can2Rate(bps);
        LogBus::printf("can2: rate %s", bps ? "set by hand" : "back to auto");
        return r->reply(200, "application/json", "{\"ok\":true}");
    });
}
#endif

// ---- the head unit's Raise link --------------------------------------------
//
// Profile messages with their bytes, arbitrary frames, keys, and what the head
// unit says back.
void WebUi::routesCanbox()
{
    // The emitter is the hu task's: its settings are read from the staged copy
    // (the newest edit) and previews are built on this handler's stack.
    route("/api/messages", HTTP_GET, [this](PsychicRequest* r) {
        App::Hu::EmitterCfg cfg;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy");
        JsonDocument d;
        JsonArray a = d["messages"].to<JsonArray>();
        for (uint8_t i = 0; i < Canbox::Rav4Emitter::messageCount(); ++i) {
            const Canbox::MsgInfo& info = Canbox::msgInfo(i);
            JsonObject o = a.add<JsonObject>();
            o["id"]       = i;
            o["name"]     = Canbox::Rav4Emitter::messageName(i);
            o["cmd"]      = Canbox::Rav4Emitter::messageCmd(i);
            o["interval"] = Canbox::Rav4Emitter::messageInterval(i);
            o["enabled"]  = i < Canbox::Rav4Emitter::MAX_MESSAGES && cfg.msgOn[i];
            o["conf"]     = Canbox::confName(info.conf);
            o["note"]     = info.note;

            // The bytes it would send now; ready=false when none of its signals
            // is known (the emitter stays silent).
            uint8_t frame[Canbox::MAX_FRAME];
            uint8_t len = 0;
            if (App::Hu::preview(i, frame, len)) {
                String hex; appendHex(hex, frame, len);
                o["ready"] = true;
                o["bytes"] = hex;
            } else {
                o["ready"] = false;
            }
        }
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // 409 when the message has no data, so nothing-sent never looks like success.
    // The hu task sends it (it owns the emitter and the UART's vehicle lane);
    // this handler waits up to 2 s for its answer.
    route("/api/messages/send", HTTP_POST, [this](PsychicRequest* r) {
        if (!_state || !_emitter) return r->reply(503, "text/plain", "not_built vehicle");
        if (!r->hasParam("id")) return r->reply(400, "text/plain", "param id");
        const uint8_t id = (uint8_t)atoi(paramStr(r, "id", "255").c_str());
        switch (App::Hu::sendNowWait(id)) {
            case App::Hu::SendNow::Sent:   break;
            case App::Hu::SendNow::NoData:
                return r->reply(409, "text/plain", "no_msg_data");
            default:
                return r->reply(503, "text/plain", "busy");
        }
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // Switching a message on or off: the registry's `huMessages` table
    // (RAM only), POST /api/settings.

    // The last frames on the head-unit UART, both directions, with ASCII.
    // 'J' rows are bytes outside any frame: a different dialect shows up there
    // while rxFrames stays 0.
    route("/api/canbox/last", HTTP_GET, [this](PsychicRequest* r) {
        if (!_huDump) return r->reply(503, "text/plain", "not_built uart_ring");
        // ?in=1: the head unit's frames alone, kept apart from ours.
        const String out = _huDump(paramStr(r, "in") == "1");
        return replyJson(r, out);
    });

    // The Raise tuning (every value the sources disagree about, persisted:
    // they are resolved in the car, where every ignition cycle is a reboot)
    // is the registry's raise.* entries: GET/POST /api/settings.

    // Send any Raise frame: cmd + payload; the framer adds the 0x2E header,
    // length and checksum. No allow-list, on purpose.
    route("/api/canbox/raw", HTTP_POST, [this](PsychicRequest* r) {
        if (!_tx) return r->reply(503, "text/plain", "not_built uart");
        if (!r->hasParam("cmd")) return r->reply(400, "text/plain", "param cmd");

        const uint32_t cmd = paramU32(r, "cmd", "", 16);
        if (cmd > 0xFF) return r->reply(400, "text/plain", "param cmd");

        uint8_t payload[Canbox::MAX_PAYLOAD];
        const int n = parseHex(paramStr(r, "data", "").c_str(), payload, sizeof(payload));
        if (n < 0) return r->reply(400, "text/plain", "param data");

        uint8_t frame[Canbox::MAX_FRAME];
        const uint8_t flen = Canbox::buildFrame((uint8_t)cmd, payload, (uint8_t)n, frame);
        if (!flen) return r->reply(400, "text/plain", "too_long");

        // The hu task's vehicle lane, never the UART from here: httpd is on
        // core 0, where WiFi can preempt it while it holds the UART mutex. The
        // lane writes it into an empty FIFO, so it cannot delay an ack.
        if (!App::Hu::post(App::Hu::Kind::Raw, frame, flen))
            return r->reply(503, "text/plain", "queue_full");

        String hex; appendHex(hex, frame, flen);
        LogBus::printf("raw tx: %s", hex.c_str());
        JsonDocument d; d["ok"] = true; d["queued"] = true; d["bytes"] = hex;
        String out; serializeJson(d, out);
        if (!out.c_str()) return r->reply(503, "text/plain", "no_memory");
        return r->reply(202, "application/json", out.c_str());
    });

    // A 0x20 key frame with a literal code, bypassing the keymap.
    // For 0x81-0x86 the second byte is a detent count, not a press state, and
    // there is no release.
    route("/api/canbox/key", HTTP_POST, [this](PsychicRequest* r) {
        if (!_keySink) return r->reply(503, "text/plain", "not_built uart");
        if (!r->hasParam("code")) return r->reply(400, "text/plain", "param code");
        const uint8_t code  = (uint8_t)paramU32(r, "code", "0", 16);
        const uint8_t state = (uint8_t)paramU32(r, "state", "1", 0);

        // A button needs its release or the head unit holds it; knobs have none.
        const bool release = !Canbox::isKnobCode(code) && paramBool(r, "release", true) && state != 0;
        // The keys task sends it, after any key already queued.
        if (!App::KeyBus::postLiteral(code, state, release))
            return r->reply(503, "text/plain", "queue_full");
        return r->reply(202, "application/json", "{\"ok\":true,\"queued\":true}");
    });
}

#if FEATURE_DISPLAY_CAN
// The body of /api/display.
void WebUi::fillDisplay(JsonObject d)
{
        d["type"]     = _cfg->s().displayType;
        d["funcReg"]  = _cfg->s().funcReg;
        d["canRx"]    = _canRx ? *_canRx : 0;
        // The panel task's copy, published after every poll(): the library
        // itself is never called from here (httpd).
        const MegaOpen::PanelStatus ps = _panel ? _panel->read() : MegaOpen::PanelStatus{};
        d["attached"] = ps.attached;
        if (ps.attached) {
            const affa::SyncState s = (affa::SyncState)ps.syncState;
            d["sync"]       = (uint8_t)s;
            JsonObject fl = d["syncFlags"].to<JsonObject>();
            fl["failed"]    = affa::hasFlag(s, affa::SyncState::Failed);
            fl["peerAlive"] = affa::hasFlag(s, affa::SyncState::PeerAlive);
            fl["start"]     = affa::hasFlag(s, affa::SyncState::Start);
            fl["funcsReg"]  = affa::hasFlag(s, affa::SyncState::FuncsReg);
            d["synced"]     = ps.synced;
            d["registered"] = ps.registered;
            d["passive"]    = ps.passive;
            d["busy"]       = ps.busy;
            d["queued"]     = ps.queued;
            d["lastResult"] = affa::resultName((affa::Result)ps.lastResult);
            JsonObject f = d["supports"].to<JsonObject>();
            f["text"]       = (ps.features & MegaOpen::PF_TEXT) != 0;
            f["time"]       = (ps.features & MegaOpen::PF_TIME) != 0;
            f["power"]      = (ps.features & MegaOpen::PF_POWER) != 0;
            f["menu"]       = (ps.features & MegaOpen::PF_MENU) != 0;
            f["popup"]      = (ps.features & MegaOpen::PF_POPUP) != 0;
            f["fullscreen"] = (ps.features & MegaOpen::PF_FULLSCREEN) != 0;
            f["confirm"]    = (ps.features & MegaOpen::PF_CONFIRM) != 0;
            f["info"]       = (ps.features & MegaOpen::PF_INFO) != 0;
        }
        // Bus counters for the panel's TwaiLink are on /api/can.
        if (ps.attached) {
            // Where the opening got to, and why the last session ended (see the
            // phase log in src/app/UiTask.cpp).
            d["phase"]       = affa::phaseName((affa::Phase)ps.phase);

            // Last RenderSlot drawn, tracked by the library. The pane never shows
            // (showNavBitmap() uses RenderSlot::None); `popup` means the popup was
            // drawn last, not that the screen under it is gone.
            static const char* const kSlot[] = {
                "text", "clock", "menu", "highlight", "popup",
                "fullscreen", "confirm", "info", "control", "none"};
            const uint8_t sl = ps.lastRendered;
            d["lastRendered"]   = kSlot[sl < 10 ? sl : 9];
            d["lastRenderedMs"] = ps.lastRenderedMs;

            // Time left before automatic repaints resume after a posted screen;
            // check this first when the media screen "stopped". 0xFFFFFFFF = never.
            // The ui task's copy, so no pointers.
            if (_ui.status) {
                const uint32_t nowMs = millis();
                const uint32_t held  = _ui.status->read().glassHeldUntilMs;
                const uint32_t hold  = _ui.glassHoldMs ? *_ui.glassHoldMs : 0;
                d["glassHoldMs"] = hold;
                d["glassHeldMs"] =
                    !held                                ? 0
                    : !hold                              ? 0xFFFFFFFFu
                    : ((int32_t)(nowMs - held) < 0)      ? (held - nowMs)
                                                         : 0;
            }

            d["sessionsLost"] = ps.sessionsLost;
            d["lastLoss"]    = affa::lossReasonName((affa::LossReason)ps.lastLoss);
            d["linkFlaps"]      = ps.linkFlaps;
            d["linkRecoveries"] = ps.linkRecoveries;
            d["linkDownMs"]     = ps.linkDownMs;
            // The panel task's pump and its TX gate (src/app/PanelOwner.h).
            d["foreignPolls"]  = ps.foreignPolls;
            d["pollLateMaxUs"] = ps.pollLateMaxUs;
            d["txGated"]       = ps.txGated;
            d["txGates"]       = ps.txGates;
            const uint32_t inst = Diag::CanBench::status().installs;
            d["reinstalls"]    = inst ? inst - 1 : 0;
            d["failStreak"]    = ps.failStreak;
        }
        if (_ops) {
            d["rendersPosted"]  = _ops->posted();
            // Renders refused at the call (the log says why), never drawn.
            d["rendersRefused"] = _ops->refused();
        }
}
#else
void WebUi::fillDisplay(JsonObject d) { d["attached"] = false; }
#endif

void WebUi::addRoute(const char* path, http_method method, PsychicHttpRequestCallback fn)
{
    route(path, method, fn);
}
