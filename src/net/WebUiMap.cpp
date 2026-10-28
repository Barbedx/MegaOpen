#include "WebUi.h"
#include "../utils/Scratch.h"
#include <math.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include "../app/CarTask.h"
#include "../app/HuTask.h"
#include "../app/CarDiag.h"
#include "../proto/canbox/Protocol.h"
#include "../cfg/CarFile.h"
#include "../cfg/CarStore.h"
#include "../utils/PsramJson.h"
#include "../proto/canbox/FieldView.h"
#include "../proto/canbox/Protocol.h"
#include "../vehicle/DecodeTable.h"
#include "../vehicle/Derived.h"
#include "../utils/LogBus.h"
#include "../utils/WallClock.h"

// ---------------------------------------------------------------------------
// /api/map — the RAV4 map (docs/design/web-ui.md, slice 7)
// ---------------------------------------------------------------------------
// One row per field of the head-unit protocol (Canbox::activeProtocol(), served
// by /api/catalog as `raise`): what binds it on the car, its value now, and a
// test send. The protocol's own settings (units, forms) are its registry rows
// (raise.*, /api/settings), not here.
//
// Ownership: the decode map is the car task's (Car::decodeTable(), staged);
// pins and tests are commands to it; the emitter and the UART are the hu
// task's (Hu::emitterCfg() staged, Hu::sendNowWait() a request). This file
// reads State (single aligned words) and the sniffer's copies, nothing else.
//
// Replaces /api/signals, /api/signals/unpinall, /api/emitter, POST /api/canbox
// and the scale fields of the old /api/canbox/tuning (now raise.* in
// /api/settings); those went with the old pages in
// slice 8.

using Canbox::TargetView;

namespace
{
    constexpr uint32_t TEST_MS      = 30000;
    constexpr uint8_t  MAX_EDITS    = 16;     // rows in one POST
    constexpr uint32_t APPLY_WAIT_MS = 300;   // a test's pin reaching State

    // One row of a POST, checked before anything is changed.
    using Edit = Vehicle::DecodeEdit;

    // snprintf's count, cut to what is in the buffer.
    size_t fit(int n, size_t cap) { return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1; }

    int bad(PsychicRequest* r, int code, const char* id, const char* why)
    {
        char b[128];
        snprintf(b, sizeof(b), "%s%s%s", id ? id : "", id ? ": " : "", why);
        return r->reply(code, "text/plain", b);
    }

    // A scaled integer with its decimal point placed: 1368, 2 -> "13.68".
    const char* dec(char* b, size_t cap, int32_t v, uint8_t d)
    {
        if (!d) { snprintf(b, cap, "%ld", (long)v); return b; }
        int32_t p = 1;
        for (uint8_t i = 0; i < d; ++i) p *= 10;
        const uint32_t a = v < 0 ? (uint32_t)-(int64_t)v : (uint32_t)v;
        snprintf(b, cap, "%s%lu.%0*lu", v < 0 ? "-" : "", (unsigned long)(a / p), (int)d, (unsigned long)(a % p));
        return b;
    }

    // ?live=1: every signal, in enum order (the meta's order), as
    // [value, ageMs, src, verified, confirm]: value null when there is none,
    // age null when CAN never set it; src c=can k=const p=pinned d=derived
    // o=off; confirm 1 = a CAN hypothesis the owner may ✓, 2 = ✓ by the owner
    // (the factory row is a hypothesis), may be taken back, 0 = neither.
    // Compact on purpose: the live page polls it every 1.5 s.
    void putLive(ChunkOut& out, const Vehicle::State& st, uint32_t now)
    {
        uint8_t how[Vehicle::SIG_COUNT], conf[Vehicle::SIG_COUNT] = {};
        for (uint8_t s = 0; s < Vehicle::SIG_COUNT; ++s) {
            const uint8_t* in = nullptr;
            how[s] = Vehicle::Derived::inputs(s, in) ? 'd' : 'o';
        }
        App::Car::decodeTable().read([&](const Vehicle::DecodeTable& t) {
            for (uint16_t i = 0; i < t.row.size(); ++i) if (t.row[i].sig < Vehicle::SIG_COUNT) how[t.row[i].sig] = 'c';
            for (uint16_t i = 0; i < t.k.size(); ++i)   if (t.k[i].sig < Vehicle::SIG_COUNT) how[t.k[i].sig] = 'k';
            for (uint8_t s = 0; s < Vehicle::SIG_COUNT; ++s)
                if (how[s] == 'c') conf[s] = t.measured(s) ? 2 : 1;
        });
        // Outside the staging mutex: the factory table is flash.
        for (uint8_t s = 0; s < Vehicle::SIG_COUNT; ++s)
            if (conf[s] == 2 && Vehicle::DecodeTable::factoryMeasured(s)) conf[s] = 0;
        char b[64], v[16], a[12];
        out.put(",\"sigs\":[");
        for (uint8_t s = 0; s < Vehicle::SIG_COUNT; ++s) {
            const char h = st.pinned(s) && how[s] != 'k' ? 'p' : (char)how[s];
            const uint32_t age = st.ageMs(s, now);
            if (age == Vehicle::State::NEVER) snprintf(a, sizeof(a), "null");
            else snprintf(a, sizeof(a), "%lu", (unsigned long)age);
            out.put(b, fit(snprintf(b, sizeof(b), "%s[%s,%s,\"%c\",%d,%u]", s ? "," : "",
                                    st.has(s) ? dec(v, sizeof(v), st.get(s), Vehicle::meta(s).decimals) : "null",
                                    a, h, Vehicle::verified(s) ? 1 : 0, (unsigned)conf[s]), sizeof(b)));
        }
        const App::Car::AlertStats as = App::Car::alertStats();
        out.put(b, fit(snprintf(b, sizeof(b), "],\"alertStats\":{\"fired\":%lu,\"shown\":%lu,\"dropped\":%lu}",
                                (unsigned long)as.fired, (unsigned long)as.shown, (unsigned long)as.dropped), sizeof(b)));
    }

    // ?meta=1: what the live page needs once — every signal's name, unit,
    // decimals, yes/no, group, range, and an empty slot where its Ukrainian
    // label was (the page has the words: k.sig, k.raise). The alert rules are a settings-registry
    // table: GET/POST /api/settings `alerts`, described by the catalog.
    void putMeta(ChunkOut& out)
    {
        char b[224], lo[16], hi[16];
        out.put(",\"meta\":[");
        for (uint8_t s = 0; s < Vehicle::SIG_COUNT; ++s) {
            const Vehicle::Meta& m = Vehicle::meta(s);
            out.put(b, fit(snprintf(b, sizeof(b), "%s[\"%s\",\"%s\",%u,%d,%u,%s,%s,\"%s\"]", s ? "," : "", m.name, m.unit,
                                    (unsigned)m.decimals, m.boolean ? 1 : 0, (unsigned)m.group,
                                    dec(lo, sizeof(lo), m.min, m.decimals), dec(hi, sizeof(hi), m.max, m.decimals),
                                    ""),   // the name: the page's (k.sig / k.raise in web/i18n)
                           sizeof(b)));
        }
        out.put("]");
    }
}

void WebUi::routesMap()
{
    // {send, uart, rows:[{id, src, can, k, v, how, left, raw}]}.
    // The field's name, unit, range and fw are the catalog's (`raise`); the
    // page joins the two by id. Values have the decimal point placed.
    route("/api/map", HTTP_GET, [this](PsychicRequest* r) {
        // ~450 B with the field map: per request (Scratch), not on httpd's 4 kB stack.
        Scratch<Canbox::HuConfig> cfgBox;
        if (!cfgBox) return r->reply(503, "text/plain", "no_memory");
        Canbox::HuConfig& cfg = *cfgBox;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy");
        const uint32_t now = millis();
        // ?live=1: the live-car page's poll, every signal and no Raise rows;
        // ?meta=1: the signals' descriptions, once.
        const bool live = r->hasParam("live"), meta = r->hasParam("meta");
        // ?sig=<name>: that one car signal's binding (the signals page's
        // editor), as one row; any signal, not only a head-unit field.
        const String one = r->hasParam("sig") ? r->getParam("sig")->value() : String();
        const uint8_t oneSig = one.length() ? Vehicle::sigByName(one.c_str()) : Vehicle::SIG_COUNT;
        if (one.length() && oneSig >= Vehicle::SIG_COUNT) return r->reply(404, "text/plain", "not_found");
        ChunkOut out(r);
        char b[320], v[16], k[16];
        out.put(b, fit(snprintf(b, sizeof(b), "{\"send\":%s,\"uart\":%s,\"rows\":[",
                            cfg.enabled ? "true" : "false", _cfg->s().uartEnabled ? "true" : "false"), sizeof(b)));
        const Canbox::Protocol& P = Canbox::activeProtocol();
        const uint8_t nRows = oneSig < Vehicle::SIG_COUNT ? 1 : P.fieldCount();
        for (uint8_t i = 0; !live && i < nRows; ++i) {
            struct { const char* id; uint8_t sig; uint8_t dec; } t;
            if (oneSig < Vehicle::SIG_COUNT) {
                const Vehicle::Meta& mt = Vehicle::meta(oneSig);
                t = {mt.name, oneSig, mt.decimals};
            } else {
                const TargetView rt = P.field(i);
                t = {rt.id, rt.sig, rt.dec};
            }
            // The binding, copied out under the staging mutex (the newest edit).
            Vehicle::DecodeRow row{};
            int32_t kv = 0;
            uint8_t src = Vehicle::SRC_OFF;
            if (t.sig < Vehicle::SIG_COUNT)
                App::Car::decodeTable().read([&](const Vehicle::DecodeTable& d) {
                    const int ri = d.rowOf(t.sig), ki = d.constOf(t.sig);
                    if (ri >= 0)      { row = d.row[(uint16_t)ri]; src = Vehicle::SRC_CAN; }
                    else if (ki >= 0) { kv = d.k[(uint16_t)ki].value; src = Vehicle::SRC_CONST; }
                });
            const uint32_t left = t.sig < Vehicle::SIG_COUNT ? App::Car::testLeftMs(t.sig, now) : 0;
            const bool has = _state && t.sig < Vehicle::SIG_COUNT && _state->has(t.sig);
            const char* how = left ? "test" : uiId(Vehicle::MAP_SRC, src);

            out.put(b, fit(snprintf(b, sizeof(b), "%s{\"id\":\"%s\",\"src\":\"%s\",\"k\":%s,\"v\":%s,\"how\":\"%s\"",
                                i ? "," : "", t.id, uiId(Vehicle::MAP_SRC, src),
                                Canbox::fmtFixed(k, sizeof(k), kv, t.dec),
                                has ? Canbox::fmtFixed(v, sizeof(v), _state->get(t.sig), t.dec) : "null", how), sizeof(b)));
            if (left) out.put(b, fit(snprintf(b, sizeof(b), ",\"left\":%lu", (unsigned long)((left + 999) / 1000)), sizeof(b)));
            // Computed by the board (avg speed, trip time, gear...): unbound, the
            // page says "board -> HU"; bound, its editor offers that to go back.
            if (t.sig < Vehicle::SIG_COUNT) {
                const uint8_t* in = nullptr;
                if (Vehicle::Derived::inputs(t.sig, in)) out.put(",\"calc\":true");
            }
            if (src == Vehicle::SRC_CAN) {
                out.put(",\"can\":");
                out.put(b, (size_t)Vehicle::DecodeJson::canJson(b, sizeof(b), row));
#if FEATURE_VEHICLE_CAN
                // The id's last frame, when the ID table (/api/can/sniff) has it.
                const bool ext = (row.flags & Vehicle::DF_EXT) != 0;
                const uint32_t key = ext ? (row.canId | 0x80000000u) : row.canId;
                Vehicle::CanSniffer::Entry e;
                for (uint8_t s = 0; _sniffer && s < _sniffer->count(); ++s) {
                    if (!_sniffer->copyAt(s, e) || !e.used || e.id != key) continue;
                    int n = snprintf(b, sizeof(b), ",\"raw\":\"0x%0*lX:", ext ? 8 : 3, (unsigned long)row.canId);
                    for (uint8_t j = 0; j < e.len && j < 8; ++j) n += snprintf(b + n, sizeof(b) - n, " %02X", e.last[j]);
                    n += snprintf(b + n, sizeof(b) - n, "\"");
                    out.put(b, (size_t)n);
                    break;
                }
#endif
            } else {
                out.put(",\"can\":null");
            }
            out.put("}");
        }
        out.put("]");
        if (live && _state) putLive(out, *_state, now);
        if (meta) putMeta(out);
        out.put("}");
        return out.finish();
    });

    // Any of: {send:bool}, {rows:[{id,src,k,can}]},
    // {confirm:{sig,on}} (the owner's ✓: that signal's CAN rows measured, or
    // a hypothesis again; reset brings the factory's flags back),
    // {reset:true} (back to decode_defaults.inc: the NVS blob is erased). Every
    // part is checked before any is applied. A row replaces whatever bound
    // that field; the map goes to the car task (no reboot) and to NVS (`decode`).
    // ---- the head-unit field map (docs/design/hu-mapping.md, step 2) ----------
    // GET: every field of the active protocol (RAV4 today): `rule`, the
    // owner's (null: none), `def`, the protocol's default (null: its own
    // signal; the fields that start off), and `v`, the value sent now. POST
    // {rows:[{id, from: "auto"|"own"|"sig"|"k"|"off", src, mul, div, add}]}:
    // "auto" drops the owner's rule (the default again); the rest set one.
    // Applied at the hu task's next pass and saved (NVS "huFmap").
    route("/api/hu/fields", HTTP_GET, [this](PsychicRequest* r) {
        if (!_state) return r->reply(503, "text/plain", "not_built vehicle");
        // ~450 B with the field map: per request (Scratch), not on httpd's 4 kB stack.
        Scratch<Canbox::HuConfig> cfgBox;
        if (!cfgBox) return r->reply(503, "text/plain", "no_memory");
        Canbox::HuConfig& cfg = *cfgBox;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy");
        Scratch<Canbox::FieldMap> eff;
        if (!eff) return r->reply(503, "text/plain", "no_memory");
        const Canbox::Protocol& P = Canbox::activeProtocol();
        P.withDefaults(cfg.fields, *eff);
        const Canbox::FieldView fv(*_state, eff.get());
        ChunkOut out(r);
        char b[220], v[16];
        auto rule = [&](const Canbox::FieldRule* ru, uint8_t) {
            if (!ru) { out.put("null"); return; }
            out.put(b, (size_t)CarFile::ruleJson(b, sizeof(b), *ru));
        };
        out.put(b, fit(snprintf(b, sizeof(b), "{\"proto\":\"%s\",\"rows\":[", P.id()), sizeof(b)));
        bool first = true;
        for (uint8_t i = 0; i < P.fieldCount(); ++i) {
            const TargetView t = P.field(i);
            if (t.sig >= Vehicle::SIG_COUNT || !t.fw) continue;
            const bool has = fv.has(t.sig);
            out.put(b, fit(snprintf(b, sizeof(b), "%s{\"id\":\"%s\",\"sig\":\"%s\",\"v\":%s,\"rule\":",
                                first ? "" : ",", t.id, Vehicle::meta(t.sig).name,
                                has ? Canbox::fmtFixed(v, sizeof(v), fv.get(t.sig), t.dec) : "null"), sizeof(b)));
            first = false;
            rule(cfg.fields.find(t.sig), t.sig);
            out.put(",\"def\":");
            rule(P.defaults().find(t.sig), t.sig);
            out.put("}");
        }
        out.put("]}");
        return out.finish();
    });

    route("/api/hu/fields", HTTP_POST, [this](PsychicRequest* r) {
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        // ~450 B with the field map: per request (Scratch), not on httpd's 4 kB stack.
        Scratch<Canbox::HuConfig> cfgBox;
        if (!cfgBox) return r->reply(503, "text/plain", "no_memory");
        Canbox::HuConfig& cfg = *cfgBox;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy");
        Canbox::FieldMap& m = cfg.fields;   // the copy in Scratch, edited
        for (JsonObjectConst row : d["rows"].as<JsonArrayConst>()) {
            const Canbox::Protocol& P = Canbox::activeProtocol();
            const int ti = P.findField(row["id"] | "");
            if (ti < 0) return r->reply(400, "text/plain", "param id");
            const uint8_t field = P.field((uint8_t)ti).sig;
            Canbox::FieldRule ru;
            bool clear = false;
            if (const char* why = CarFile::ruleFrom(row, field, ru, clear)) {
                char w[48];
                snprintf(w, sizeof(w), "param %.*s", (int)(strchr(why, ':') ? strchr(why, ':') - why : strlen(why)), why);
                return r->reply(400, "text/plain", w);
            }
            if (clear) { m.clear(field); continue; }
            if (!m.set(ru)) return r->reply(400, "text/plain", "rows_max 29");
        }
        // The protocol's defaults go into the same 32-rule map as the owner's
        // (withDefaults): room for both, or a rule would be dropped silently.
        if (m.count() > Canbox::FieldMap::MAX - Canbox::activeProtocol().defaults().count())
            return r->reply(400, "text/plain", "rows_max 29");
        const bool staged = App::Hu::emitterCfg().edit([&](Canbox::HuConfig& c) { c.fields = m; });
        if (!staged) return r->reply(503, "text/plain", "busy");
        const bool saved = CarStore::saveFields(Canbox::activeProtocol(), m);
        LOGI("hu", "field map: %u rule(s)%s", (unsigned)m.count(), saved ? "" : ", NOT saved");
        return r->reply(200, "application/json", saved ? "{\"ok\":true}" : "{\"ok\":true,\"saved\":false}");
    });

    route("/api/map", HTTP_POST, [this](PsychicRequest* r) {
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");

        // ~450 B with the field map: per request (Scratch), not on httpd's 4 kB stack.
        Scratch<Canbox::HuConfig> cfgBox;
        if (!cfgBox) return r->reply(503, "text/plain", "no_memory");
        Canbox::HuConfig& cfg = *cfgBox;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy");

        // ---- check ----
        const bool hasSend = d["send"].is<bool>();
        const bool send = d["send"] | false;
        if (hasSend && send && !_cfg->s().uartEnabled) return r->reply(409, "text/plain", "uart_off");


        const bool reset = d["reset"] | false;

        // {confirm:{sig, on}}: the owner's ✓ on «Авто — наживо» after a car
        // check: the signal's CAN rows measured (on) or a hypothesis again.
        const bool hasConf = !d["confirm"].isNull();
        uint8_t confSig = Vehicle::SIG_COUNT;
        const bool confOn = d["confirm"]["on"] | true;
        if (hasConf) {
            confSig = Vehicle::sigByName(d["confirm"]["sig"] | "");
            if (confSig >= Vehicle::SIG_COUNT) return r->reply(404, "text/plain", "param confirm");
            if (!d["confirm"]["on"].isNull() && !d["confirm"]["on"].is<bool>())
                return r->reply(400, "text/plain", "param confirm.on");
        }

        Edit ed[MAX_EDITS];
        uint8_t n = 0;
        JsonArrayConst rows = d["rows"].as<JsonArrayConst>();
        if (rows.size() > MAX_EDITS) return r->reply(400, "text/plain", "param rows");
        for (JsonObjectConst o : rows) {
            // Any car signal by its name (a head-unit field's id is its
            // signal's): the binding is the signal's, wherever it is edited.
            const char* id = o["id"] | "";
            const uint8_t sig = Vehicle::sigByName(id);
            if (sig >= Vehicle::SIG_COUNT) return bad(r, 404, nullptr, "not_found");
            const Vehicle::Meta& mt = Vehicle::meta(sig);
            struct { uint8_t sig; int32_t min, max; uint8_t dec; } t{sig, mt.min, mt.max, mt.decimals};
            const int s = uiFind(Vehicle::MAP_SRC, o["src"] | "");
            if (s < 0) return bad(r, 400, id, "param src");   // can, const or off
            for (uint8_t j = 0; j < n; ++j)
                if (ed[j].sig == t.sig) return bad(r, 400, id, "twice");
            Edit& e = ed[n++];
            e.sig = t.sig;
            e.src = (uint8_t)s;
            e.k = 0;
            e.row = Vehicle::DecodeRow{};
            if (s == Vehicle::SRC_CAN) {
                if (!o["can"].is<JsonObjectConst>()) return bad(r, 400, id, "param can");
                if (const char* why = Vehicle::DecodeJson::canFrom(o["can"], e.row)) return bad(r, 400, id, why);
            } else if (s == Vehicle::SRC_CONST) {
                if (!o["k"].is<double>() || !Canbox::toFixed(o["k"].as<double>(), t.dec, e.k) ||
                    e.k < t.min || e.k > t.max) {
                    char lo[16], hi[16], w[48];
                    snprintf(w, sizeof(w), "k: between %s %s", Canbox::fmtFixed(lo, sizeof(lo), t.min, t.dec),
                             Canbox::fmtFixed(hi, sizeof(hi), t.max, t.dec));
                    return bad(r, 400, id, w);
                }
            }
        }

        // ---- apply ----
        // The map first, all or nothing (DecodeTable::apply builds it on a
        // copy): a refused request has changed nothing.
        if (reset || n || hasConf) {
            // Static: only this handler stages the map (its rows are in PSRAM).
            static Vehicle::DecodeTable scratch;
            Vehicle::DecodeChange c;
            c.reset = reset; c.ed = ed; c.n = n;
            c.hasConf = hasConf; c.confSig = confSig; c.confOn = confOn;
            const char* why = nullptr;
            if (!App::Car::decodeTable().edit([&](Vehicle::DecodeTable& t) { why = t.apply(c, scratch); }))
                return r->reply(503, "text/plain", "busy");
            if (why) return r->reply(409, "text/plain", why);
        }

        if (hasSend) {
            if (!App::Hu::emitterCfg().edit([&](Canbox::HuConfig& c) { c.enabled = send; cfg = c; }))
                return r->reply(503, "text/plain", "busy");
            _cfg->saveHuSend(send);
            LOGI("map", "send to the head unit %s", send ? "on" : "off");
        }

        if (reset || n || hasConf) {
            // The car file: the newest table, copied under the staging mutex
            // (the car task only try-takes it), written after. Factory alone =
            // no `decode` in the file.
            const char* why = nullptr;
            if (!n && !hasConf) {
                if (!CarStore::factoryDecode()) why = "file_failed";
            } else {
                Scratch<Vehicle::DecodeTable> copy;
                if (!copy) why = "no_memory";
                else if (!App::Car::decodeTable().read([&](const Vehicle::DecodeTable& t) { *copy = t; }))
                    why = "busy";
                else if (!CarStore::saveDecode(*copy)) why = "file_failed";
            }
            if (hasConf)
                LOGI("map", "%s %s", Vehicle::meta(confSig).name,
                               confOn ? "confirmed in the car (measured)" : "back to a hypothesis");
            if (reset || n)
                LOGI("map", "%s: %u field(s)%s%s", reset ? "back to factory" : "saved", (unsigned)n,
                               why ? ", NOT in the car file: " : "", why ? why : "");
            else if (why)
                LOGI("map", "NOT in the car file: %s", why);
            if (why) return bad(r, 500, nullptr, why);   // applied until the next boot only
        }

        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // {id, value}: the value is pinned for 30 s (RAM only, a pin wins over
    // CAN) and the field's message goes to the head unit now, once. With
    // `send` on it keeps going out on its schedule until the 30 s are up.
    route("/api/map/test", HTTP_POST, [this](PsychicRequest* r) {
        if (!_state) return r->reply(503, "text/plain", "not_built vehicle");
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        const char* id = d["id"] | "";
        const Canbox::Protocol& P = Canbox::activeProtocol();
        const int ti = P.findField(id);
        if (ti < 0) return bad(r, 404, nullptr, "not_found");
        const TargetView t = P.field((uint8_t)ti);
        if (!t.fw) return bad(r, 409, id, "no_field_112");
        if (!_cfg->s().uartEnabled) return r->reply(409, "text/plain", "uart_off");
        // ~450 B with the field map: per request (Scratch), not on httpd's 4 kB stack.
        Scratch<Canbox::HuConfig> cfgBox;
        if (!cfgBox) return r->reply(503, "text/plain", "no_memory");
        Canbox::HuConfig& cfg = *cfgBox;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy");
        if (const char* why = P.messageBlocked(t.msg, cfg)) return bad(r, 409, id, why);
        // A field that is off (the owner's rule, or one that starts off: an
        // unknown frame to the tablet) is not sent, test or not.
        const Canbox::FieldRule* fr = P.ruleFor(cfg.fields, t.sig);
        if (fr && fr->kind == Canbox::FK_OFF) return bad(r, 409, id, "field_off");

        int32_t v = 0;
        if (!d["value"].is<double>() || !Canbox::toFixed(d["value"].as<double>(), t.dec, v) ||
            v < t.min || v > t.max || (t.boolean && v != 0 && v != 1)) {
            char lo[16], hi[16], w[48];
            snprintf(w, sizeof(w), "value: between %s %s", Canbox::fmtFixed(lo, sizeof(lo), t.min, t.dec),
                     Canbox::fmtFixed(hi, sizeof(hi), t.max, t.dec));
            return bad(r, 400, id, w);
        }

        // The car task pins it; the message is built from State, so wait
        // until the pin is there.
        App::Car::test(t.sig, v, TEST_MS);
        const uint32_t t0 = millis();
        while (!(_state->pinned(t.sig) && _state->get(t.sig) == v)) {
            if ((uint32_t)(millis() - t0) > APPLY_WAIT_MS)
                return r->reply(503, "text/plain", "busy");
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        switch (App::Hu::sendNowWait(t.msg)) {
            case App::Hu::SendNow::Sent: break;
            // Trips ride on the odometer's 0x7D 04, tyres on "TPMS є", hybrid on
            // "Гібрид є", the rear radar on "Радар увімкнено": the message goes
            // only once its gate has a value.
            case App::Hu::SendNow::NoData:
                return bad(r, 409, id, "needs_main");   // its message needs its main field too (odometer, radar on, TPMS, hybrid)
            default: return r->reply(503, "text/plain", "busy");
        }

        char hex[3 * Canbox::HU_FRAME_MAX + 1];
        hex[0] = '\0';
        uint8_t frame[Canbox::HU_FRAME_MAX], len = 0;
        if (App::Hu::preview(t.msg, frame, len))
            for (unsigned i = 0, o = 0; i < len; ++i) o += (unsigned)snprintf(hex + o, sizeof(hex) - o, i ? " %02X" : "%02X", frame[i]);
        char vs[16];
        LOGI("map", "test %s = %s -> %s", id, Canbox::fmtFixed(vs, sizeof(vs), v, t.dec), hex);
        char b[sizeof(hex) + 24];
        snprintf(b, sizeof(b), "{\"ok\":true,\"bytes\":\"%s\"}", hex);
        return r->reply(200, "application/json", b);
    });
}

// ---------------------------------------------------------------------------
// The map in the settings file (GET/POST /api/config, WebUi.cpp)
// ---------------------------------------------------------------------------
// `decode` and `hu` as the car file holds them (CarFile: the bindings, the
// constants, the owner's ✓, the field map, all by signal name), plus hu.send.
// A file without them leaves both alone. A 1.6.x file's base64 `decode` is
// refused (decode: not_object): its rows are signal indexes, not names.

void WebUi::mapToFile(ChunkOut& out)
{
    // The car file's own shape (CarFile): rows by signal name. ~20 kB of
    // document, in PSRAM, per request.
    Scratch<Vehicle::DecodeTable> t;
    Scratch<Canbox::HuConfig> c;
    if (!t || !c) return;   // no memory: the keys are left out (an import keeps its own)
    const bool haveT = App::Car::decodeTable().read([&](const Vehicle::DecodeTable& x) { *t = x; });
    const bool haveC = App::Hu::emitterCfg().peek(*c);
    JsonDocument car(&PsramJson::instance());
    JsonObject o = car.to<JsonObject>();
    if (haveT) CarFile::putDecode(o, *t);
    if (haveC) {
        CarFile::putFields(o, Canbox::activeProtocol(), c->fields);
        o["hu"]["send"] = c->enabled;
    }
    // serializeJson into a String replaces it: the key goes out on its own.
    String s;
    if (haveT) { out.put(",\"decode\":"); serializeJson(o["decode"], s); out.put(s.c_str(), s.length()); }
    if (haveC) { out.put(",\"hu\":");     serializeJson(o["hu"], s);     out.put(s.c_str(), s.length()); }
}

const char* WebUi::mapFromFile(JsonObjectConst file, bool dry, bool& changedMap, bool& changedHu)
{
    changedMap = changedHu = false;
    // Static: only the config POST uses it (httpd is one task).
    static Vehicle::DecodeTable table;
    if (!file["decode"].isNull()) {
        if (!file["decode"].is<JsonObjectConst>()) return "decode: not_object";
        uint16_t skipped = 0;
        if (const char* why = CarFile::getDecode(file, table, skipped)) return why;
        if (skipped) LOGW("map", "settings file: %u decode row(s) name a signal this firmware does not know: skipped",
                          (unsigned)skipped);
        changedMap = true;
    }

    Scratch<Canbox::HuConfig> cBox;   // not on httpd's stack (see the routes)
    if (!cBox) return "no_memory";
    Canbox::HuConfig& c = *cBox;
    JsonObjectConst hu = file["hu"].as<JsonObjectConst>();
    if (!file["hu"].isNull()) {
        if (hu.isNull()) return "hu: not_object";
        if (!App::Hu::emitterCfg().peek(c)) return "busy";
        if (hu["send"].is<bool>()) c.enabled = hu["send"].as<bool>() && _cfg->s().uartEnabled;
        // The active protocol's map, if the file has one; another
        // protocol's entry is not this board's to apply.
        if (!hu[Canbox::activeProtocol().id()].isNull()) {
            uint16_t skipped = 0;
            if (const char* why = CarFile::getFields(file, Canbox::activeProtocol(), c.fields, skipped)) return why;
        }
        changedHu = true;
    }
    if (dry) return nullptr;

    if (changedMap) {
        if (!App::Car::decodeTable().edit([&](Vehicle::DecodeTable& t) { t = table; })) return "busy";
        if (!CarStore::saveDecode(table)) return "file_failed";
        LOGI("map", "set from a settings file: %u rows, %u constants", (unsigned)table.row.size(), (unsigned)table.k.size());
    }
    if (changedHu) {
        // Send and the field map only: the protocol's tuning comes with the
        // file's settings (raise.*), applied on their own.
        if (!App::Hu::emitterCfg().edit([&](Canbox::HuConfig& e) { e.enabled = c.enabled; e.fields = c.fields; }))
            return "busy";
        _cfg->saveHuSend(c.enabled);
        if (!CarStore::saveFields(Canbox::activeProtocol(), c.fields)) return "file_failed";
        LOGI("map", "head unit set from a settings file: send %s, %u field rule(s)", c.enabled ? "on" : "off",
                       (unsigned)c.fields.count());
    }
    return nullptr;
}

#if FEATURE_DIAG
// ---------------------------------------------------------------------------
// /api/diag — the climate ECU's 21 C1 on CAN A (src/app/CarDiag.h)
// ---------------------------------------------------------------------------
// GET: the switches, what it is doing, the last reply. POST {on?, interval?,
// probe?}: on/interval are saved (switching on clears a latched bus fault);
// probe = one request now, its reply in the log.
void WebUi::routesDiag()
{
    route("/api/diag", HTTP_GET, [](PsychicRequest* r) {
        App::CarDiag::View v;
        App::CarDiag::view(v);
        char hex[3 * sizeof(v.reply) + 1] = "";
        size_t k = 0;
        for (uint8_t i = 0; i < v.replyLen && k + 4 < sizeof(hex); ++i)
            k += snprintf(hex + k, sizeof(hex) - k, "%s%02X", i ? " " : "", v.reply[i]);
        JsonDocument d;
        d["on"]        = v.on;
        d["session"]   = v.session;
        d["inSession"] = v.inSession;
        d["interval"]  = v.intervalS;
        d["state"]     = v.state;
        d["last"]      = v.last;
        d["nrc"]       = v.nrc;
        d["reply"]     = hex;
        d["lastAgoMs"] = v.lastAgoMs;
        d["ok"]        = v.ok;
        d["timeouts"]  = v.timeouts;
        d["negatives"] = v.negatives;
        d["bad"]       = v.bad;
        d["sent"]      = v.sent;
        d["tx"]        = v.tx;
        if (v.haveValues) {
            d["cabin"] = v.cabinX10 / 10.0;
            d["sun"]   = v.sunWm2;
        }
        String out;
        serializeJson(d, out);
        return replyJson(r, out);
    });

    route("/api/diag", HTTP_POST, [](PsychicRequest* r) {
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        App::CarDiag::View v;
        App::CarDiag::view(v);
        const bool hasOn = d["on"].is<bool>(), hasIv = d["interval"].is<int>();
        if (hasOn || hasIv) {
            const int iv = hasIv ? d["interval"].as<int>() : v.intervalS;
            if (iv < App::CarDiag::INTERVAL_MIN_S || iv > App::CarDiag::INTERVAL_MAX_S)
                return r->reply(400, "text/plain", "param interval");
            if (const char* why = App::CarDiag::set(hasOn ? d["on"].as<bool>() : v.on, (uint8_t)iv))
                return r->reply(400, "text/plain", why);
        }
        if (d["session"].is<bool>())
            if (const char* why = App::CarDiag::setSession(d["session"].as<bool>()))
                return r->reply(400, "text/plain", why);
        if (d["probe"] | false) App::CarDiag::probe();
        if (d["sweep"] | false) App::CarDiag::sweep();
        return r->reply(200, "application/json", "{\"ok\":true}");
    });
}
#endif

// ---------------------------------------------------------------------------
// /api/alerts — the panel alerts for the phone and tablet apps (voice)
// ---------------------------------------------------------------------------
// GET ?since=N: the alerts shown after number N (the last 16 at most), oldest
// first, and `last`, the newest number. `upMs` lets an app see a reboot (the
// numbers start again from 1). POST /api/alerts/test {sig?}: a test entry.
void WebUi::routesAlerts()
{
    // GET /api/gears: the gear card on the car page, polled once a second.
    // Live: speed (km/h x100), rpm, ratio (km/h per 1000 rpm x10), gear,
    // clutch, neutral (null when the signal has no value); the table in use;
    // the learner's peaks [ratio, seconds] this boot.
    route("/api/gears", HTTP_GET, [this](PsychicRequest* r) {
        JsonDocument d;
        auto sig = [&](const char* key, uint8_t s) {
            if (_state && _state->has(s)) d[key] = _state->get(s);
            else d[key] = nullptr;
        };
        sig("speed", Vehicle::SIG_SPEED);
        sig("rpm", Vehicle::SIG_RPM);
        sig("ratio", Vehicle::SIG_GEAR_RATIO);
        sig("gear", Vehicle::SIG_GEAR);
        sig("clutch", Vehicle::SIG_CLUTCH);
        sig("neutral", Vehicle::SIG_NEUTRAL);
        JsonArray t = d["table"].to<JsonArray>();
        for (int i = 0; i < 6; ++i) t.add(_cfg->s().gearRatio[i]);
        uint16_t pr[6];
        uint32_t ps[6];
        const uint8_t n = App::Car::gearPeaks(pr, ps, 6);
        JsonArray pk = d["peaks"].to<JsonArray>();
        for (uint8_t i = 0; i < n; ++i) {
            JsonArray e = pk.add<JsonArray>();
            e.add(pr[i]);
            e.add(ps[i]);
        }
        String out;
        serializeJson(d, out);
        return replyJson(r, out);
    });

    route("/api/alerts", HTTP_GET, [](PsychicRequest* r) {
        const uint32_t since = r->hasParam("since") ? (uint32_t)strtoul(r->getParam("since")->value().c_str(), nullptr, 10) : 0;
        // t: the app's clock (ms since 1970), the board's date (src/utils/WallClock.h).
        if (r->hasParam("t")) WallClock::fromApp(strtoll(r->getParam("t")->value().c_str(), nullptr, 10), "wifi");
        static App::Car::AlertEntry list[App::Car::ALERT_LOG];   // httpd is one task
        uint32_t last = 0;
        const uint8_t n = App::Car::alertsSince(since, list, App::Car::ALERT_LOG, last);
        const uint32_t now = millis();
        JsonDocument d;
        d["last"] = last;
        d["upMs"] = now;
        JsonArray a = d["alerts"].to<JsonArray>();
        for (uint8_t i = 0; i < n; ++i) {
            JsonObject o = a.add<JsonObject>();
            o["seq"]   = list[i].seq;
            o["agoMs"] = now - list[i].ms;
            o["sig"]   = Vehicle::meta(list[i].sig).name;
            o["rule"]  = list[i].row == 0xFF ? -1 : (int)list[i].row;
            o["value"] = list[i].value;
            o["text"]  = list[i].text;
        }
        String out;
        serializeJson(d, out);
        return replyJson(r, out);
    });

    // GET ?name=drive_no_lights: one signal now, {"name","has","value"}, for
    // an app that repeats a reminder while its condition lasts.
    route("/api/signal", HTTP_GET, [this](PsychicRequest* r) {
        if (!r->hasParam("name")) return r->reply(400, "text/plain", "param name");
        const String name = r->getParam("name")->value();
        const uint8_t sig = Vehicle::sigByName(name.c_str());
        if (sig >= Vehicle::SIG_COUNT) return r->reply(404, "text/plain", "param name");
        const bool has = _state && _state->has(sig);
        char out[96];
        snprintf(out, sizeof(out), "{\"name\":\"%s\",\"has\":%s,\"value\":%ld}", Vehicle::meta(sig).name,
                 has ? "true" : "false", has ? (long)_state->get(sig) : 0L);
        return replyJson(r, String(out));
    });

    route("/api/alerts/test", HTTP_POST, [](PsychicRequest* r) {
        JsonDocument d;
        deserializeJson(d, r->body());
        const char* name = d["sig"] | "drive_no_lights";
        const uint8_t sig = Vehicle::sigByName(name);
        if (sig >= Vehicle::SIG_COUNT) return r->reply(400, "text/plain", "param sig");
        char text[48];
        Vehicle::AlertEval::text(text, sizeof(text), sig, 1, true);
        App::Car::injectAlert(sig, 1, text);
        return r->reply(200, "application/json", "{\"ok\":true}");
    });
}
