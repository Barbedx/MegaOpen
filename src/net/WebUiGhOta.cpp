#include "WebUi.h"
#include "GhOta.h"
#if FEATURE_GH_OTA
#include <WiFi.h>
#include <ElegantOTA.h>
#include "../Version.h"

// ---------------------------------------------------------------------------
// /api/fw — firmware updates from GitHub Releases (src/net/GhOta.h)
// ---------------------------------------------------------------------------
// GET is the state the Плата page polls; check, install and reboot are POST
// and only ever start or release a job, so httpd never waits on the network.
// Never automatic, and never with the car moving (GhOta::moving()).

static int fwBusy(PsychicRequest* r, const char* why)
{
    JsonDocument d;
    d["error"] = why;
    String out;
    serializeJson(d, out);
    return r->reply(409, "application/json", out.c_str());
}

void WebUi::routesGhOta()
{
    route("/api/fw", HTTP_GET, [](PsychicRequest* r) {
        GhOta::View v;
        GhOta::view(v);
        JsonDocument d;
        d["fw"]     = MEGAOPEN_VERSION;
        d["env"]    = MegaOpen::envName();
        d["phase"]  = GhOta::phaseId(v.phase);
        d["latest"] = v.latest;
        d["asset"]  = v.asset;
        d["size"]   = v.size;
        d["got"]    = v.got;
        if (v.err[0]) d["err"] = v.err;
        d["heapStart"]  = v.heapStart;
        d["heapMin"]    = v.heapMin;
        d["heapCreated"] = v.heapCreated;
        d["heapTask"]   = v.heapTask;
        d["heapClient"] = v.heapClient;
        d["heapTls"]    = v.heapTls;
        d["stackFree"]  = v.stackFree;
        JsonArray sm = d["stepMin"].to<JsonArray>();
        JsonArray st = d["stepMs"].to<JsonArray>();
        for (int i = 0; i < 10; ++i) { sm.add(v.stepMin[i]); st.add(v.stepMs[i]); }
        d["tlsInPsram"] = v.tlsInPsram;
        String out;
        serializeJson(d, out);
        return replyJson(r, out);
    });

    route("/api/fw/check", HTTP_POST, [](PsychicRequest* r) {
        if (!WiFi.isConnected())
            return fwBusy(r, "fw_ap");   // on its own AP: join a WiFi network with internet
        const char* why = GhOta::check();
        return why ? fwBusy(r, why) : r->reply(200, "application/json", "{\"ok\":true}");
    });

    route("/api/fw/install", HTTP_POST, [this](PsychicRequest* r) {
        if (!WiFi.isConnected())
            return fwBusy(r, "fw_ap");   // on its own AP: join a WiFi network with internet
        GhOta::attachCar(_state);
        const char* why = GhOta::install();
        return why ? fwBusy(r, why) : r->reply(200, "application/json", "{\"ok\":true}");
    });

    // After a good install the board waits for a standing car or this.
    route("/api/fw/reboot", HTTP_POST, [this](PsychicRequest* r) {
        GhOta::attachCar(_state);
        const char* why = GhOta::reboot();
        return why ? fwBusy(r, why) : r->reply(200, "application/json", "{\"ok\":true}");
    });

    // Only reached when gateLocalOta()'s filter turned ElegantOTA's own
    // handlers away (PsychicHttp hands a filtered request to onNotFound).
    // The file upload on the page shows this text as its error.
    auto refused = [](PsychicRequest* r) {
        GhOta::View v;
        GhOta::view(v);
        return fwBusy(r, v.phase == GhOta::Phase::Written
                             ? "fw_written"     // a GitHub update waits for its reboot
                             : "fw_running");   // a GitHub check or download is running
    };
    route("/ota/start", HTTP_GET, refused);
    route("/ota/upload", HTTP_POST, refused);
}

void WebUi::gateLocalOta()
{
    // One OTA owner: ElegantOTA's routes stand down while a GitHub job runs or
    // waits to reboot into its image (their filter, WebUi::gateOta()), and a
    // GitHub install asks whether a local session is alive.
    ElegantOTA.onStart([]() { GhOta::localStart(); });
    ElegantOTA.onProgress([](size_t, size_t) { GhOta::localChunk(); });
    ElegantOTA.onEnd([](bool) { GhOta::localEnd(); });
}
#endif
