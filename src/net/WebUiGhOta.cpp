#include "WebUi.h"
#include "GhOta.h"
#if FEATURE_GH_OTA
#include <WiFi.h>
#include "../Version.h"

// ---------------------------------------------------------------------------
// /api/fw — firmware updates from GitHub Releases (src/net/GhOta.h)
// ---------------------------------------------------------------------------
// GET is the state the Плата page polls; check and install are POST and only
// ever start a job, so httpd never waits on the network. Never automatic, and
// never with the car moving: install refuses while the speed is fresh and
// above a walk.

static constexpr int32_t  MOVING_KMH_X100 = 300;    // 3 km/h
static constexpr uint32_t SPEED_FRESH_MS  = 5000;

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
        d["tlsInPsram"] = v.tlsInPsram;
        String out;
        serializeJson(d, out);
        return replyJson(r, out);
    });

    route("/api/fw/check", HTTP_POST, [](PsychicRequest* r) {
        if (!WiFi.isConnected())
            return fwBusy(r, "the board is on its own access point: join it to a WiFi network with internet");
        const char* why = GhOta::check();
        return why ? fwBusy(r, why) : r->reply(200, "application/json", "{\"ok\":true}");
    });

    route("/api/fw/install", HTTP_POST, [this](PsychicRequest* r) {
        if (!WiFi.isConnected())
            return fwBusy(r, "the board is on its own access point: join it to a WiFi network with internet");
        if (_state && _state->fresh(Vehicle::SIG_SPEED, millis(), SPEED_FRESH_MS) &&
            _state->get(Vehicle::SIG_SPEED) > MOVING_KMH_X100)
            return fwBusy(r, "the car is moving: update when parked");
        const char* why = GhOta::install();
        return why ? fwBusy(r, why) : r->reply(200, "application/json", "{\"ok\":true}");
    });
}
#endif
