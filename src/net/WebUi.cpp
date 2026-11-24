#include "WebUi.h"
#include "../utils/HeapSentinel.h"
#include "../link/AppLink.h"
#include "RescuePage.h"
#include "lwip/sockets.h"
#include "../cfg/SettingsJson.h"
#include "../proto/canbox/Protocol.h"
#include "Catalog.h"
#include "../cfg/SettingsRegistry.h"
#include "../keys/KeyDests.h"
#include "GhOta.h"
#include "LogPush.h"
#include "../utils/AllocWatch.h"
#include <ArduinoJson.h>
#include <ElegantOTA.h>
#include <WiFi.h>
#include <driver/twai.h>
#include <soc/twai_struct.h>   // TWAI.error_code_capture_reg, for /api/can/ecc
#include <driver/gpio.h>
#include "../diag/CanBench.h"
#include "../board/BoardProfile.h"
#include "../display/PanelRole.h"
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
#include <esp_system.h>
#include "../utils/Scratch.h"
#include "../utils/JsonEscape.h"
#include "../utils/Hex.h"
#if FEATURE_BLE && FEATURE_ANCS
#include "../apple_notification_service.h"
#endif

using namespace Keys;
#include "WebUiKit.h"


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


uint8_t* ChunkOut::s_buf = nullptr;

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
        const LogLine p = LogLine::parse(s_line, (uint32_t)len);
        if (p.level > (uint8_t)Log::sinkLevel(Log::SINK_WEB)) continue;
        out.put(h, snprintf(h, sizeof(h), "%s{\"n\":%lu,\"t\":%lu,\"l\":\"%s\",\"tag\":\"%s\",\"msg\":\"",
                            lines ? "," : "", (unsigned long)(c.n - 1), (unsigned long)ms,
                            Log::LEVEL_NAMES[p.level], Log::TAG_ROWS[p.tag].tag));
        Json::escape(s_line + p.msg, (size_t)(len - (int)p.msg),
                     [](const char* s, size_t n, void* c) { ((ChunkOut*)c)->put(s, n); }, &out);
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
    _server.config.stack_size    = App::Plan::HTTPD_STACK;

    // A phone that stops reading holds httpd in send() for this long, not the
    // default 5 s. Seconds.
    _server.config.send_wait_timeout = 2;
    // A settings file carries the car file (decode rows and formulas by
    // name): past the library's 16 kB with a full set (128 formulas alone
    // are ~38 kB). The body is buffered by malloc, in PSRAM at this size.
    _server.maxRequestBodySize = 64 * 1024;

    // [mem] marks per step of begin() (serial only; memMark is app/BootLog's).
    auto mem = [](const char* what) {
        LOGI("mem", "web.%-8s free=%6u big=%6u\n", what,
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    };
    mem("start");
    {
        Cfg::Config::Guard g(*_cfg);
        snprintf(_bootName, sizeof(_bootName), "%s", _cfg->s().mdnsName.c_str());
    }
    _server.listen(80);
    mem("listen");
    // No Nagle on the server's sockets: httpd sends the headers and the body
    // as two writes, and the second waited for the ACK of the first. Over the
    // USB link the peer is lwIP itself (127.0.0.1), whose delayed ACK made
    // every small reply 250 ms (docs/design/usb-link.md).
    _server.onOpen([](PsychicClient* c) {
        int one = 1;
        setsockopt(c->socket(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    });
    routes();
    // The one handler for the whole table — see Route in WebUi.h.
    _server.onNotFound([this](PsychicRequest* r) { return dispatch(r); });
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
        LOGI("web", "%u route(s) did not fit the table — raise ROUTE_MAX\n", _routesLost);
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
    // The name is the boot's copy: nothing renames it (it is not in the
    // registry), so no lock here.
    const http_method m = r->method();
    return RequestGate::check(r->host().c_str(), _bootName,
                              m == HTTP_GET || m == HTTP_HEAD, r->path().c_str(),
                              r->header(RequestGate::HEADER).c_str());
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

#if HEAP_SENTINEL
// The diagnostic build (src/utils/HeapSentinel.h): a heap walk before and
// after every route, the step naming the request.
static volatile uint32_t s_huntReqs = 0;
static char s_huntLast[48] = "";
static void huntCheck(const char* when, const char* path)
{
    char w[112];
    snprintf(w, sizeof(w), "web %s %s (req %u, last %s)", when, path, (unsigned)s_huntReqs, s_huntLast);
    HeapSentinel::check(w);
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
        LOGI("web", "refused %s %s: %s", http_method_str(m), path.c_str(),
                       why == RequestGate::Why::Host ? "host" : "no X-MegaOpen");
        if (why == RequestGate::Why::Host) {
            // Say where it IS reachable: the address this request came in on.
            static char w[96];
            const String ip = r->client()->localIP().toString();
            snprintf(w, sizeof(w), "%s http://%s/ http://%s.local/", RequestGate::whyText(why),
                     ip.c_str(), _bootName[0] ? _bootName : "megaopen");
            return r->reply(403, "text/plain", w);
        }
        return r->reply(403, "text/plain", RequestGate::whyText(why));
    }
#if HEAP_SENTINEL
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
#if FEATURE_GH_OTA
    GhOta::localTick();
#endif
}

void WebUi::routes()
{
#if FEATURE_WEB_PAGE
    // The app (web/, tools/gen_pages.py). Its screens are hash routes.
    route("/", HTTP_GET, [](PsychicRequest* r) {
        return replyPage(r, PAGE_APP_GZ, PAGE_APP_GZ_LEN);
    });
    // The page's English words (web/i18n/en.json); Ukrainian is in the page.
    route("/i18n/en.json", HTTP_GET, [](PsychicRequest* r) {
        return replyPage(r, PAGE_I18N_EN_GZ, PAGE_I18N_EN_GZ_LEN, "application/json");
    });
#else
    // No settings page on the board: MegaOpen Companion and the WebUSB page
    // carry it (web/ bundled). "/" keeps a firmware update and the WiFi.
    // The header tells an app with its own copy of the page to use that one.
    route("/", HTTP_GET, [](PsychicRequest* r) {
        PsychicResponse resp(r);
        resp.setCode(200);
        resp.setContentType("text/html");
        resp.addHeader("X-MegaOpen-Page", "rescue");
        resp.setContent(RESCUE_PAGE);
        return resp.send();
    });
#endif

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
    // The tags seen this boot (they register on first use) and their own
    // level, null = the global one: the «Журнал» card's per-tag rows.
    // The page's own errors: a screen that failed to open, an uncaught
    // exception. One line at WARN under [page], so the flash log and the
    // push carry what the owner saw; on 2026-11-01 «невідоме значення…» on
    // «Сигнали» in the car left no trace anywhere, the error lived in the
    // phone's browser alone. Form fields: where (the screen), msg, req (the
    // request that failed, if one did). At most one a second; folded to one
    // line and cut, whatever the page sends.
    route("/api/page/error", HTTP_POST, [](PsychicRequest* r) {
        static uint32_t s_lastMs = 0;
        const uint32_t now = millis();
        if (now - s_lastMs < 1000) return r->reply(200, "text/plain", "OK");
        s_lastMs = now;
        String msg = paramStr(r, "msg"), req = paramStr(r, "req"), where = paramStr(r, "where");
        auto fold = [](String& s, unsigned cap) {
            s.replace('\n', ' ');
            s.replace('\r', ' ');
            if (s.length() > cap) s.remove(cap);
        };
        fold(msg, 160); fold(req, 80); fold(where, 24);
        if (!msg.length()) return r->reply(400, "text/plain", "param msg");
        LOGW("page", "%s: %s%s%s", where.c_str(), msg.c_str(), req.length() ? " @ " : "", req.c_str());
        return r->reply(200, "text/plain", "OK");
    });

    route("/api/logs/tags", HTTP_GET, [](PsychicRequest* r) {
        JsonDocument d(&PsramJson::instance());
        JsonArray a = d["tags"].to<JsonArray>();
        const uint8_t n = Log::tagCount();
        for (uint8_t i = 0; i < n; ++i) {
            JsonObject o = a.add<JsonObject>();
            o["tag"] = Log::tagName(i);
            const uint8_t ov = Log::tagOverride(i);
            if (ov < Log::LEVELS) o["lvl"] = Log::LEVEL_NAMES[ov];
            else o["lvl"] = nullptr;
        }
        d["spec"] = Log::tagSpec();
        return replyDoc(r, d);
    });

    route("/api/logs", HTTP_GET, [](PsychicRequest* r) {
        if (r->hasParam("since")) return logLines(r, paramU32(r, "since", "0", 10));
        JsonDocument d(&PsramJson::instance());
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
        char p[LogFile::PATH_CAP];
        for (int n = 0; n < LogFile::FILES; ++n) {
            if (!LogFile::path(n, p, sizeof(p))) continue;
            JsonObject f = files.add<JsonObject>();
            f["n"]    = n;   // 0 = this boot, 1 = the boot before, ...
            f["size"] = LogFile::size(n);
        }
#endif
        return replyDoc(r, d);
    });

#if FEATURE_LOGFS
    // /api/logs/file?n=1 — one boot's log as text. n=0 is still being written.
    route("/api/logs/file", HTTP_GET, [](PsychicRequest* r) {
        const int n = (int)paramU32(r, "n", "0", 10);
        // This boot's file: an explicit dump, so the RAM buffer goes to flash
        // first (the LogBus task writes it; this waits up to 500 ms).
        if (n == 0) LogBus::flushAndWait(500);
        char p[LogFile::PATH_CAP];
        if (!LogFile::path(n, p, sizeof(p))) return r->reply(404, "text/plain", "no_log");
        PsychicFileResponse resp(r, LittleFS, p, "text/plain", false);
        return resp.send();
    });
#endif

    route("/api/status", HTTP_GET, [this](PsychicRequest* r) {
        JsonDocument d(&PsramJson::instance());
        statusJson(d.to<JsonObject>());
        return replyDoc(r, d);
    });

    routesSettings();        // WebUiSettings.cpp: keymap, settings, the config file


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
        LOGI("sys", "reboot requested from the web");
        return replyAndRestart(r, "{\"ok\":true}");
    });

    // ---- does it all work together -------------------------------------------
    // Each entry is a chain with one verdict and, when not "ok", the link that
    // broke it.
    route("/api/health", HTTP_GET, [this](PsychicRequest* r) {
        JsonDocument d(&PsramJson::instance());
        healthJson(d.to<JsonObject>());
        return replyDoc(r, d);
    });

#if FEATURE_BLE && FEATURE_ANCS
    // Cached ANCS notifications, raw UTF-8 (the panel gets the ASCII-folded copy).
    route("/api/notifications", HTTP_GET, [this](PsychicRequest* r) {
        JsonDocument d(&PsramJson::instance());
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
        return replyDoc(r, d);
    });
#endif

    // ---- key routing ---------------------------------------------------------
    // Where a press goes, per destination, as a class mask: the registry's
    // route* entries (GET/POST /api/settings, staged for the keys task);
    // each destination's mask and liveness is the keys chain of /api/health.

    routesWifi();            // WebUiSettings.cpp


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
#if FEATURE_DIAG
    routesDiag();
#endif
    routesAlerts();
#if FEATURE_RESISTIVE_KEYS
    routesRes();
#endif
#if FEATURE_KNOBS
    routesKnobs();
#endif

    routesCanbox();
    routesMap();
    routesRules();

    // Wipes NVS including the WiFi credentials: the board returns in AP mode at
    // http://192.168.4.1/ and the password is gone. Requires ?confirm=factory.
    route("/api/factory", HTTP_POST, [this](PsychicRequest* r) {
        if (paramStr(r, "confirm") != "factory")
            return r->reply(400, "application/json",
                            "{\"ok\":false,\"error\":\"confirm factory\"}");
        LOGI("cfg", "FACTORY RESET — NVS wiped, rebooting into AP mode");
        _cfg->factoryReset();
        return replyAndRestart(r, "{\"ok\":true,\"note\":\"AP mode after reboot\"}");
    });
}
