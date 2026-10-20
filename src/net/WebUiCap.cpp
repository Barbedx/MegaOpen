#include "WebUi.h"
#if FEATURE_CAPTURE
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "../capture/CanRecorder.h"
#include "../capture/CapFormat.h"

// ---------------------------------------------------------------------------
// /api/cap — the CAN capture recorder (docs/design/event-tasks.md, Step 5b)
// ---------------------------------------------------------------------------
// Everything goes through Capture::'s own calls: httpd never touches the ring,
// the chunk or the file being written. A download streams from flash in
// PsychicFileResponse's chunks, never the whole file in RAM, and holds the
// file (beginRead/endRead) so the ring cannot delete it mid-stream.
//
// Reads are GET; anything that changes state is POST, so a browser's prefetch
// cannot delete a drive. mark is both: typing /api/cap/mark?t=fuel+3/4 into a
// phone's address bar while parked is the point of it.

static String capParam(PsychicRequest* r, const char* name, const char* dflt = "")
{
    return r->hasParam(name) ? r->getParam(name)->value() : String(dflt);
}

static bool capFlag(PsychicRequest* r, const char* name, bool dflt)
{
    if (!r->hasParam(name)) return dflt;
    const String v = r->getParam(name)->value();
    return v == "1" || v == "true" || v == "on";
}

// "0012" or "12"; 0 when absent or not a number.
static uint32_t capFileNum(PsychicRequest* r)
{
    const String v = capParam(r, "f");
    char* end = nullptr;
    const unsigned long n = strtoul(v.c_str(), &end, 10);
    return (end && end != v.c_str() && (*end == 0 || strcmp(end, ".bin") == 0)) ? (uint32_t)n : 0;
}

static int capRes(PsychicRequest* r, Capture::Res res)
{
    switch (res) {
        case Capture::Res::OK:        return r->reply(200, "application/json", "{\"ok\":true}");
        case Capture::Res::NOT_FOUND: return r->reply(404, "text/plain", "no such capture file");
        case Capture::Res::BUSY:      return r->reply(409, "text/plain", "file is being written or downloaded");
        default:                      return r->reply(500, "text/plain", "failed");
    }
}

static void capSettingsOut(JsonObject o, const Capture::Settings& s)
{
    o["ring"]        = s.ring;
    o["fill"]        = s.fillPct;
    o["fileMB"]      = s.fileMB;
    o["autoProtect"] = s.autoProtect;
    o["mode"]        = uiId(CapFormat::MODE_NAMES, s.mode);
    o["autostart"]   = s.autostart;
}

static int capOk(PsychicRequest* r, const char* key, uint32_t n)
{
    char b[48];
    snprintf(b, sizeof(b), "{\"ok\":true,\"%s\":%lu}", key, (unsigned long)n);
    return r->reply(200, "application/json", b);
}

void WebUi::routesCap()
{
    // ---- status ------------------------------------------------------------------
    route("/api/cap", HTTP_GET, [](PsychicRequest* r) {
        const Capture::Status s = Capture::status();
        JsonDocument d;
        d["ready"]     = s.ready;
        d["recording"] = s.recording;
        d["mode"]      = uiId(CapFormat::MODE_NAMES, s.mode);
        d["frames"]    = s.frames;      // pushed since boot
        d["written"]   = s.written;     // records written (change-only: far fewer)
        d["dropped"]   = s.dropped;     // ring full: must stay 0 at bus rate
        d["markers"]   = s.markers;
        d["ringUsed"]  = s.ringUsed;
        d["ringCap"]   = s.ringCap;
        d["file"]      = s.curFile;     // 0 = none open
        d["fileBytes"] = s.curBytes;
        d["session"]   = s.session;
        d["capBytes"]  = s.capBytes;
        d["budget"]    = s.budget;      // what /cap may use: (fs - log) x fill %
        d["free"]      = s.budget > s.capBytes ? s.budget - s.capBytes : 0;
        d["fsFree"]    = s.fsFree;
        d["rateFps"]   = s.rateFps;
        d["ids"]       = s.ids;
        d["filterOverflow"] = s.filterOverflow;
        d["files"]     = s.files;
        JsonArray br = d["bitrate"].to<JsonArray>();
        br.add(s.bitrate[0]);           // car
        br.add(s.bitrate[1]);           // panel
        d["error"] = s.lastError ? s.lastError : "";
        capSettingsOut(d["settings"].to<JsonObject>(), Capture::settings());
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // ---- control -------------------------------------------------------------------
    route("/api/cap/start", HTTP_POST, [](PsychicRequest* r) {
        Capture::start();
        return r->reply(200, "application/json", "{\"ok\":true}");
    });
    route("/api/cap/stop", HTTP_POST, [](PsychicRequest* r) {
        Capture::stop();
        return r->reply(200, "application/json", "{\"ok\":true}");
    });
    auto mark = [](PsychicRequest* r) {
        const String t = capParam(r, "t");
        Capture::mark(t.c_str());
        return r->reply(200, "application/json", "{\"ok\":true}");
    };
    route("/api/cap/mark", HTTP_GET, mark);
    route("/api/cap/mark", HTTP_POST, mark);

    // ---- files -----------------------------------------------------------------------
    route("/api/cap/list", HTTP_GET, [](PsychicRequest* r) {
        static Capture::FileInfo files[128];   // httpd is one task: one caller
        const size_t total = Capture::list(files, 128);
        const size_t n = total < 128 ? total : 128;
        JsonDocument d;
        JsonArray a = d["files"].to<JsonArray>();
        for (size_t i = 0; i < n; ++i) {
            JsonObject o = a.add<JsonObject>();
            char name[12];
            snprintf(name, sizeof(name), "%04lu", (unsigned long)files[i].num);
            o["f"]         = name;
            o["session"]   = files[i].session;
            o["part"]      = files[i].part;
            o["size"]      = files[i].size;
            o["wall"]      = files[i].wallSec;   // unix s at open; 0 = clock unknown
            o["protected"] = files[i].prot;
            o["current"]   = files[i].current;
        }
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // ?f=NNNN: streamed in chunks; the live file is flushed first.
    route("/api/cap/get", HTTP_GET, [](PsychicRequest* r) {
        const uint32_t num = capFileNum(r);
        if (!num) return r->reply(400, "text/plain", "f=NNNN");
        char path[24];
        const Capture::Res res = Capture::beginRead(num, path, sizeof(path));
        if (res != Capture::Res::OK) return capRes(r, res);
        esp_err_t err;
        {
            PsychicFileResponse resp(r, LittleFS, path, "application/octet-stream", true);
            err = resp.send();
        }   // the file is closed here, before the hold is released
        Capture::endRead(num);
        return err;
    });

    route("/api/cap/delete", HTTP_POST, [](PsychicRequest* r) {
        const uint32_t num = capFileNum(r);
        if (!num) return r->reply(400, "text/plain", "f=NNNN");
        return capRes(r, Capture::remove(num));
    });
    route("/api/cap/delete_unprotected", HTTP_POST, [](PsychicRequest* r) {
        return capOk(r, "deleted", Capture::removeUnprotected());
    });
    // ?f=NNNN&on=0|1: the file's whole session.
    route("/api/cap/protect", HTTP_POST, [](PsychicRequest* r) {
        const uint32_t num = capFileNum(r);
        if (!num) return r->reply(400, "text/plain", "f=NNNN&on=0|1");
        return capRes(r, Capture::protect(num, capFlag(r, "on", true)));
    });
    // Every /cap file, protected ones too. The log is not touched. ?confirm=cap
    route("/api/cap/format", HTTP_POST, [](PsychicRequest* r) {
        if (capParam(r, "confirm") != "cap")
            return r->reply(400, "text/plain", "deletes every capture, protected too: add confirm=cap");
        return capOk(r, "deleted", Capture::formatArea());
    });

    // The settings are registry entries (cap.*): GET/POST /api/settings.
}
#endif
