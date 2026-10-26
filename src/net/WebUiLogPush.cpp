#include "WebUi.h"
#include "LogPush.h"
#if FEATURE_LOG_PUSH

// ---------------------------------------------------------------------------
// /api/logpush — the log to a private GitHub repository (src/net/LogPush.h)
// ---------------------------------------------------------------------------
// GET is the state the Плата page shows; never the token, only whether one is
// stored. POST {on?, token?, repo?, now?}: saves what it carries (an empty
// token keeps the stored one); now = push at the next pass when online.

void WebUi::routesLogPush()
{
    route("/api/logpush", HTTP_GET, [](PsychicRequest* r) {
        LogPush::View v;
        LogPush::view(v);
        JsonDocument d;
        d["state"]    = LogPush::stateId(v.state);
        d["on"]       = v.on;
        d["hasToken"] = v.hasToken;
        d["repo"]     = v.repo;
        d["device"]   = v.device;
        if (v.err[0]) d["err"] = v.err;
        d["status"]   = v.lastStatus;
        d["files"]    = v.files;
        d["sent"]     = v.sentBytes;
        d["lost"]     = v.lostBytes;
        d["queued"]   = v.queuedBytes;
        d["spool"]    = v.spoolBytes;
        d["okAgoMs"]  = v.lastOkAgoMs;
        d["nextInMs"] = v.nextInMs;
        d["heapMin"]  = v.heapMin;
        d["can"]      = v.canOn;
        d["canMB"]    = v.canMB;
        d["canToday"] = v.canToday;
        d["canFile"]  = v.canFile;
        d["canBudget"] = v.canBudget;
        String out;
        serializeJson(d, out);
        return replyJson(r, out);
    });

    route("/api/logpush", HTTP_POST, [](PsychicRequest* r) {
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        bool on = false;
        const bool hasOn = d["on"].is<bool>();
        if (hasOn) on = d["on"].as<bool>();
        const char* tok  = d["token"].is<const char*>() ? d["token"].as<const char*>() : nullptr;
        const char* repo = d["repo"].is<const char*>() ? d["repo"].as<const char*>() : nullptr;
        if (hasOn || tok || repo) {
            const char* why = LogPush::set(hasOn ? &on : nullptr, tok, repo);
            if (why) return r->reply(400, "text/plain", why);
        }
        const bool hasCan = d["can"].is<bool>();
        const bool can = hasCan && d["can"].as<bool>();
        // From the page's field it may come as text.
        const bool hasMB = d["canMB"].is<int>() || d["canMB"].is<const char*>();
        uint32_t mb = 0;
        if (d["canMB"].is<int>()) mb = d["canMB"].as<int>() > 0 ? (uint32_t)d["canMB"].as<int>() : 0;
        else if (hasMB) mb = (uint32_t)strtoul(d["canMB"].as<const char*>(), nullptr, 10);
        if (hasCan || hasMB) {
            const char* why = LogPush::setCan(hasCan ? &can : nullptr, hasMB ? &mb : nullptr);
            if (why) return r->reply(400, "text/plain", why);
        }
        if (d["now"] | false) LogPush::pushSoon();
        return r->reply(200, "application/json", "{\"ok\":true}");
    });
}
#endif
