#include "WebUi.h"
#include "../utils/LogBus.h"
#include "../app/KeyBus.h"
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// /api/macros — combinations that mean something the keypad has no button for
// ---------------------------------------------------------------------------
// A config document and a small live one (polled twice a second). The lists
// (actions, triggers, effects) are the catalog's (/api/catalog).
//
// Validated here and again in the engine: the engine never matches an invalid
// macro (protects old blobs); this endpoint refuses to store one and says why.
//
// The table is staged (docs/design/event-tasks.md, Step 2): the keys task is the
// engine's only caller, so the page edits a staged MacroTable and the keys
// task swaps it in. The live half (fires, progress, keys down) is read from the
// engine itself: counters, one writer, a stale value at worst.

using namespace Keys;

static int macNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Same contract as WebUi.cpp's parseHex (a static there).
static int macParseHex(const char* s, uint8_t* out, uint8_t maxLen)
{
    uint8_t n = 0;
    int hi = -1;
    for (; s && *s; ++s) {
        if (*s == ' ' || *s == ',' || *s == ':' || *s == '-') continue;
        const int v = macNibble(*s);
        if (v < 0) return -1;
        if (hi < 0) { hi = v; continue; }
        if (n >= maxLen) return -1;
        out[n++] = (uint8_t)((hi << 4) | v);
        hi = -1;
    }
    return hi >= 0 ? -1 : (int)n;
}

void WebUi::routesMacros()
{
    // ---- the whole configuration -------------------------------------------
    route("/api/macros", HTTP_GET, [this](PsychicRequest* r) {
        if (!_macros || !_st.macros) return r->reply(503, "text/plain", "not_built macro_engine");
        JsonDocument d;

        JsonArray ms = d["macros"].to<JsonArray>();
        // In place under the staging mutex: eight Macros do not belong on a
        // 4 kB httpd stack. The newest edit, applied or not.
        const bool got = _st.macros->read([&](const MacroTable& tab) {
            for (uint8_t i = 0; i < MacroEngine::count(); ++i) {
                const Macro& m = tab.m[i];
                JsonObject o = ms.add<JsonObject>();
                o["enabled"]  = m.enabled;
                o["name"]     = m.name;
                o["trigger"]  = triggerName(m.trigger);
                JsonArray k = o["keys"].to<JsonArray>();
                for (uint8_t j = 0; j < Macro::MAX_MEMBERS; ++j) k.add(name(m.keys[j]));
                o["taps"]     = m.taps;
                o["windowMs"] = m.windowMs;
                o["swallow"]  = m.swallow;
                o["effect"]   = effectName(m.effect);
                o["action"]   = name(m.action);
                // RAV4 key code, Raise command byte or message index: hex everywhere.
                char hex[5];
                snprintf(hex, sizeof(hex), "%02X", m.code);
                o["codeHex"]  = hex;
                String data;
                for (uint8_t j = 0; j < m.dataLen; ++j) {
                    char t[4]; snprintf(t, sizeof(t), "%02X", m.data[j]);
                    if (j) data += ' ';
                    data += t;
                }
                o["data"]     = data;
                o["arg"]      = m.arg;
                o["valid"]    = m.valid();
                o["fires"]    = _macros->at(i).fires;
                o["progress"] = _macros->progress(i);
            }
        });
        if (!got) return r->reply(503, "text/plain", "busy");

        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // ---- the live half, polled -------------------------------------------
    route("/api/macros/live", HTTP_GET, [this](PsychicRequest* r) {
        if (!_macros) return r->reply(503, "text/plain", "not_built macro_engine");
        JsonDocument d;
        d["fired"]     = _macros->fired();
        d["swallowed"] = _macros->swallowed();
        d["replayed"]  = _macros->replayed();

        // Keys held right now, by name: the key stream only shows events.
        const uint32_t down = _macros->downMask();
        JsonArray dn = d["down"].to<JsonArray>();
        for (uint8_t i = 0; i < (uint8_t)Action::_Count; ++i)
            if (down & (1u << i)) dn.add(name((Action)i));

        JsonArray pg = d["progress"].to<JsonArray>();
        JsonArray fr = d["fires"].to<JsonArray>();
        for (uint8_t i = 0; i < MacroEngine::count(); ++i) {
            pg.add(_macros->progress(i));
            fr.add(_macros->at(i).fires);
        }

        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // The whole table at once, never a single row: macros interact.
    route("/api/macros", HTTP_POST, [this](PsychicRequest* r) {
        if (!_macros || !_cfg || !_st.macros) return r->reply(503, "text/plain", "not_built macro_engine");
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        JsonArray in = d["macros"].as<JsonArray>();
        if (in.isNull()) return r->reply(400, "text/plain", "param macros");

        // One row, parsed. Shared by both passes so what is validated is what
        // is stored.
        auto parse = [](JsonObject o, Macro& m) -> bool {
            m = Macro{};
            m.enabled  = o["enabled"] | false;
            m.trigger  = triggerFromName(o["trigger"] | "off");
            m.effect   = effectFromName(o["effect"] | "none");
            m.action   = fromName(o["action"] | "None");
            m.swallow  = o["swallow"] | true;
            m.taps     = (uint8_t)(o["taps"] | 3);
            m.windowMs = (uint16_t)(o["windowMs"] | 600);

            // Compacted: an empty middle select is skipped, not stored as None.
            // `members` is derived from the keys, never posted.
            JsonArray k = o["keys"].as<JsonArray>();
            m.members = 0;
            if (!k.isNull())
                for (JsonVariant v : k) {
                    if (m.members >= Macro::MAX_MEMBERS) break;
                    const Action a = fromName(v.as<const char*>());
                    if (a != Action::None) m.keys[m.members++] = a;
                }

            m.code = (uint8_t)strtoul(o["codeHex"] | "0", nullptr, 16);

            const int dn = macParseHex(o["data"] | "", m.data, Macro::MAX_DATA);
            if (dn < 0) return false;
            m.dataLen = (uint8_t)dn;

            strncpy(m.arg,  o["arg"]  | "", Macro::ARG_LEN - 1);
            strncpy(m.name, o["name"] | "", Macro::NAME_LEN - 1);
            m.arg[Macro::ARG_LEN - 1] = '\0';
            m.name[Macro::NAME_LEN - 1] = '\0';
            return true;
        };

        // Two passes; the first stores nothing, so a table is never half-applied.
        // (Staging in an array would put eight Macros on a 4 kB HTTP worker stack.)
        {
            Macro m;
            uint8_t i = 0;
            for (JsonObject o : in) {
                if (i >= MacroEngine::count()) break;
                char why[96];
                if (!parse(o, m)) {
                    snprintf(why, sizeof(why), "%u: macro_hex %u", (unsigned)i, (unsigned)Macro::MAX_DATA);
                    return r->reply(400, "text/plain", why);
                }
                // An enabled row that can never fire is refused; a disabled
                // one is a draft and may be incomplete.
                if (m.enabled && !m.valid()) {
                    snprintf(why, sizeof(why), "%u: macro_incomplete", (unsigned)i);
                    return r->reply(400, "text/plain", why);
                }
                ++i;
            }
        }

        // Into the staged table, in place. The keys task applies it: it keeps
        // each row's fire count (not configuration) and drops half-collected
        // sequences and mutes, or a key muted by a deleted macro stays muted.
        // Serialised from inside, so NVS gets exactly what was staged, and
        // written after: a flash write must not hold the staging mutex the
        // keys task try-takes.
        uint8_t blob[MacroEngine::BLOB_SIZE];
        uint8_t on = 0;
        const bool ok = _st.macros->edit([&](MacroTable& t) {
            uint8_t i = 0;
            for (JsonObject o : in) {
                if (i >= MacroEngine::count()) break;
                parse(o, t.m[i]);
                ++i;
            }
            // Rows the page did not send are rows that no longer exist.
            for (uint8_t j = i; j < MacroEngine::count(); ++j) t.m[j] = Macro{};
            for (uint8_t j = 0; j < MacroEngine::count(); ++j)
                if (t.m[j].enabled) ++on;
            MacroEngine::toBlob(t, blob);
        });
        if (!ok) return r->reply(503, "text/plain", "busy");
        App::KeyBus::kick();
        _cfg->saveMacroBlob(blob);
        LOGI("macro", "saved — %u active", (unsigned)on);
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    route("/api/macros/defaults", HTTP_POST, [this](PsychicRequest* r) {
        if (!_macros || !_cfg || !_st.macros) return r->reply(503, "text/plain", "not_built macro_engine");
        // As above: serialised under the mutex, written after it.
        uint8_t blob[MacroEngine::BLOB_SIZE];
        if (!_st.macros->edit([&](MacroTable& t) { t.loadDefaults(); MacroEngine::toBlob(t, blob); }))
            return r->reply(503, "text/plain", "busy");
        App::KeyBus::kick();
        _cfg->saveMacroBlob(blob);
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // Run the effect only, no trigger: splits "combo not recognised" from
    // "effect wrong".
    route("/api/macros/test", HTTP_POST, [this](PsychicRequest* r) {
        if (!_macros) return r->reply(503, "text/plain", "not_built macro_engine");
        if (!r->hasParam("id")) return r->reply(400, "text/plain", "param id");
        const uint8_t id = (uint8_t)atoi(r->getParam("id")->value().c_str());
        if (id >= MacroEngine::count()) return r->reply(400, "text/plain", "param id");
        if (_macros->at(id).effect == Effect::None)
            return r->reply(409, "text/plain", "row_no_effect");
        // The keys task runs it, after any key already queued. 202: queued.
        if (!App::KeyBus::postMacroRun(id))
            return r->reply(503, "text/plain", "queue_full");
        return r->reply(202, "application/json", "{\"ok\":true,\"queued\":true}");
    });
}
