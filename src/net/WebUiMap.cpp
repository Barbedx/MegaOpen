#include "WebUi.h"
#include <math.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include "../app/CarTask.h"
#include "../app/HuTask.h"
#include "../app/CarDiag.h"
#include "../proto/canbox/RaiseTargets.h"
#include "../proto/canbox/FieldView.h"
#include "../vehicle/DecodeTable.h"
#include "../vehicle/Derived.h"
#include "../utils/LogBus.h"
#include "../utils/WallClock.h"

// ---------------------------------------------------------------------------
// /api/map — the RAV4 map (docs/design/web-ui.md, slice 7)
// ---------------------------------------------------------------------------
// One row per Raise field (Canbox::RaiseTargets, served by /api/catalog as
// `raise`): what binds it on the car, its value now, and a test send.
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
namespace RT = Canbox::RaiseTargets;

namespace
{
    constexpr uint32_t TEST_MS      = 30000;
    constexpr uint8_t  MAX_EDITS    = 16;     // rows in one POST
    constexpr uint32_t APPLY_WAIT_MS = 300;   // a test's pin reaching State

    // One row of a POST, checked before anything is changed.
    using Edit = Vehicle::DecodeEdit;

    // The 0x28 form the emitter is set to send must be the one a field is in.
    const char* formWhy(uint8_t msg, uint8_t cmd28)
    {
        if (msg == Canbox::Rav4Emitter::MSG_TEMP && cmd28)
            return "form_ac";     // 0x28 carries the A/C form now: switch it in Service, Raise
        if (msg == Canbox::Rav4Emitter::MSG_AC && !cmd28)
            return "form_temp";   // 0x28 carries the outside temperature now
        return nullptr;
    }

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
            for (uint8_t i = 0; i < t.nRows; ++i) if (t.row[i].sig < Vehicle::SIG_COUNT) how[t.row[i].sig] = 'c';
            for (uint8_t i = 0; i < t.nK; ++i)    if (t.k[i].sig < Vehicle::SIG_COUNT) how[t.k[i].sig] = 'k';
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
    // {send, uart, units:{fuel,trip}, rows:[{id, src, can, k, v, how, left, raw}]}.
    // The field's name, unit, range and fw are the catalog's (`raise`); the
    // page joins the two by id. Values have the decimal point placed.
    route("/api/map", HTTP_GET, [this](PsychicRequest* r) {
        App::Hu::EmitterCfg cfg;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy");
        const uint32_t now = millis();
        // ?live=1: the live-car page's poll, every signal and no Raise rows;
        // ?meta=1: the signals' descriptions, once.
        const bool live = r->hasParam("live"), meta = r->hasParam("meta");
        ChunkOut out(r);
        char b[200], v[16], k[16];
        out.put(b, fit(snprintf(b, sizeof(b), "{\"send\":%s,\"uart\":%s,\"units\":{\"fuel\":%u,\"trip\":%u},\"exp\":%u,\"rows\":[",
                            cfg.enabled ? "true" : "false", _cfg->s().uartEnabled ? "true" : "false",
                            (unsigned)cfg.tuning.fuelUnit, (unsigned)cfg.tuning.tripUnit, (unsigned)cfg.exp), sizeof(b)));
        for (uint8_t i = 0; !live && i < RT::count(); ++i) {
            const TargetView t = RT::at(i);
            // The binding, copied out under the staging mutex (the newest edit).
            Vehicle::DecodeRow row{};
            int32_t kv = 0;
            uint8_t src = Vehicle::SRC_OFF;
            if (t.sig < Vehicle::SIG_COUNT)
                App::Car::decodeTable().read([&](const Vehicle::DecodeTable& d) {
                    const int ri = d.rowOf(t.sig), ki = d.constOf(t.sig);
                    if (ri >= 0)      { row = d.row[ri]; src = Vehicle::SRC_CAN; }
                    else if (ki >= 0) { kv = d.k[ki].value; src = Vehicle::SRC_CONST; }
                });
            const uint32_t left = t.sig < Vehicle::SIG_COUNT ? App::Car::testLeftMs(t.sig, now) : 0;
            const bool has = _state && t.sig < Vehicle::SIG_COUNT && _state->has(t.sig);
            const char* how = left ? "test" : uiId(Vehicle::MAP_SRC, src);

            out.put(b, fit(snprintf(b, sizeof(b), "%s{\"id\":\"%s\",\"src\":\"%s\",\"k\":%s,\"v\":%s,\"how\":\"%s\"",
                                i ? "," : "", t.id, uiId(Vehicle::MAP_SRC, src),
                                RT::fmt(k, sizeof(k), kv, t.dec),
                                has ? RT::fmt(v, sizeof(v), _state->get(t.sig), t.dec) : "null", how), sizeof(b)));
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

    // Any of: {send:bool}, {units:{fuel,trip}}, {exp:n}, {rows:[{id,src,k,can}]},
    // {confirm:{sig,on}} (the owner's ✓: that signal's CAN rows measured, or
    // a hypothesis again; reset brings the factory's flags back),
    // {reset:true} (back to decode_defaults.inc: the NVS blob is erased). Every
    // part is checked before any is applied. A row replaces whatever bound
    // that field; the map goes to the car task (no reboot) and to NVS (`decode`).
    // ---- the head-unit field map (docs/design/hu-mapping.md, step 2) ----------
    // GET: every field of the active protocol (RAV4 today), its rule (null =
    // its own signal) and the value it sends now. POST {rows:[{id, from:
    // "auto"|"sig"|"k"|"off", src, mul, div, add}]}: rules set or cleared,
    // applied at the hu task's next pass and saved (NVS "huFmap").
    route("/api/hu/fields", HTTP_GET, [this](PsychicRequest* r) {
        if (!_state) return r->reply(503, "text/plain", "not_built vehicle");
        App::Hu::EmitterCfg cfg;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy");
        ChunkOut out(r);
        char b[220], v[16];
        out.put("{\"proto\":\"raise112\",\"rows\":[");
        for (uint8_t i = 0; i < RT::count(); ++i) {
            const TargetView t = RT::at(i);
            if (t.sig >= Vehicle::SIG_COUNT) continue;
            const Canbox::FieldView fv(*_state, &cfg.fields);
            const bool has = fv.has(t.sig);
            out.put(b, fit(snprintf(b, sizeof(b), "%s{\"id\":\"%s\",\"sig\":\"%s\",\"v\":%s,\"rule\":",
                                i ? "," : "", t.id, Vehicle::meta(t.sig).name,
                                has ? RT::fmt(v, sizeof(v), fv.get(t.sig), t.dec) : "null"), sizeof(b)));
            const Canbox::FieldRule* ru = cfg.fields.find(t.sig);
            if (!ru) out.put("null}");
            else {
                static const char* const FROM[] = {"", "sig", "k", "off"};
                out.put(b, fit(snprintf(b, sizeof(b), "{\"from\":\"%s\",\"src\":\"%s\",\"mul\":%d,\"div\":%d,\"add\":%ld}}",
                                    FROM[ru->kind <= 3 ? ru->kind : 0],
                                    ru->kind == Canbox::FK_SIGNAL && ru->src < Vehicle::SIG_COUNT ? Vehicle::meta(ru->src).name : "",
                                    (int)ru->mul, (int)ru->div, (long)ru->add), sizeof(b)));
            }
        }
        out.put("]}");
        return out.finish();
    });

    route("/api/hu/fields", HTTP_POST, [this](PsychicRequest* r) {
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        App::Hu::EmitterCfg cfg;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy");
        Canbox::FieldMap m = cfg.fields;
        for (JsonObjectConst row : d["rows"].as<JsonArrayConst>()) {
            const int ti = RT::find(row["id"] | "");
            if (ti < 0) return r->reply(400, "text/plain", "param id");
            const uint8_t field = RT::at((uint8_t)ti).sig;
            const char* from = row["from"] | "auto";
            if (!strcmp(from, "auto")) { m.clear(field); continue; }
            Canbox::FieldRule ru;
            ru.field = field;
            if (!strcmp(from, "off")) ru.kind = Canbox::FK_OFF;
            else if (!strcmp(from, "k")) { ru.kind = Canbox::FK_CONST; ru.add = row["add"] | 0; }
            else if (!strcmp(from, "sig")) {
                ru.kind = Canbox::FK_SIGNAL;
                ru.src = Vehicle::sigByName(row["src"] | "");
                if (ru.src >= Vehicle::SIG_COUNT) return r->reply(400, "text/plain", "param src");
                ru.mul = (int16_t)(row["mul"] | 1);
                ru.div = (int16_t)(row["div"] | 1);
                ru.add = row["add"] | 0;
            } else return r->reply(400, "text/plain", "param from");
            if (ru.kind == Canbox::FK_SIGNAL && !ru.div) return r->reply(400, "text/plain", "param div");
            if (!m.set(ru)) return r->reply(400, "text/plain", "rows_max 32");
        }
        const bool staged = App::Hu::emitterCfg().edit([&](App::Hu::EmitterCfg& c) { c.fields = m; });
        if (!staged) return r->reply(503, "text/plain", "busy");
        const bool saved = _cfg->saveFieldMap(m);
        LOGI("hu", "field map: %u rule(s)%s", (unsigned)m.count(), saved ? "" : ", NOT saved");
        return r->reply(200, "application/json", saved ? "{\"ok\":true}" : "{\"ok\":true,\"saved\":false}");
    });

    route("/api/map", HTTP_POST, [this](PsychicRequest* r) {
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");

        App::Hu::EmitterCfg cfg;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy");

        // ---- check ----
        const bool hasSend = d["send"].is<bool>();
        const bool send = d["send"] | false;
        if (hasSend && send && !_cfg->s().uartEnabled) return r->reply(409, "text/plain", "uart_off");

        // {exp: n}: the head-unit experiments, Rav4Emitter::Exp bits.
        const bool hasExp = !d["exp"].isNull();
        const int exp = d["exp"] | -1;
        if (hasExp && (!d["exp"].is<int>() || exp < 0 || exp > Canbox::Rav4Emitter::EXP_ALL))
            return r->reply(400, "text/plain", "param exp");
        // Two experiments in one field are refused, not settled behind the
        // page's back (the emitter would keep the battery).
        if (hasExp)
            if (const char* why = Canbox::Rav4Emitter::experimentsWhy((uint8_t)exp))
                return r->reply(400, "text/plain", why);

        const bool hasUnits = d["units"].is<JsonObject>();
        int fuel = cfg.tuning.fuelUnit, trip = cfg.tuning.tripUnit;
        if (hasUnits) {
            fuel = d["units"]["fuel"] | fuel;
            trip = d["units"]["trip"] | trip;
            bool fok = false, tok = false;
            for (const auto& u : Canbox::FUEL_UNITS) fok |= u.v == fuel;
            for (const auto& u : Canbox::TRIP_UNITS) tok |= u.v == trip;
            if (!fok) return r->reply(400, "text/plain", "param units.fuel");
            if (!tok) return r->reply(400, "text/plain", "param units.trip");
        }

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
            const char* id = o["id"] | "";
            const int ti = RT::find(id);
            if (ti < 0) return bad(r, 404, nullptr, "not_found");
            const TargetView t = RT::at((uint8_t)ti);
            if (t.sig >= Vehicle::SIG_COUNT) return bad(r, 409, id, "no_signal");   // the firmware has no signal for it yet
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
                if (!o["k"].is<double>() || !RT::toInt(o["k"].as<double>(), t.dec, e.k) ||
                    e.k < t.min || e.k > t.max) {
                    char lo[16], hi[16], w[48];
                    snprintf(w, sizeof(w), "k: between %s %s", RT::fmt(lo, sizeof(lo), t.min, t.dec),
                             RT::fmt(hi, sizeof(hi), t.max, t.dec));
                    return bad(r, 400, id, w);
                }
            }
        }

        // ---- apply ----
        // The map first, all or nothing (DecodeTable::apply builds it on a
        // copy): a refused request has changed nothing.
        if (reset || n || hasConf) {
            // Static: 2.4 kB, and only this handler stages the map.
            static Vehicle::DecodeTable scratch;
            Vehicle::DecodeChange c;
            c.reset = reset; c.ed = ed; c.n = n;
            c.hasConf = hasConf; c.confSig = confSig; c.confOn = confOn;
            const char* why = nullptr;
            if (!App::Car::decodeTable().edit([&](Vehicle::DecodeTable& t) { why = t.apply(c, scratch); }))
                return r->reply(503, "text/plain", "busy");
            if (why) return r->reply(409, "text/plain", why);
        }

        if (hasSend || hasUnits || hasExp) {
            if (!App::Hu::emitterCfg().edit([&](App::Hu::EmitterCfg& c) {
                    if (hasSend) c.enabled = send;
                    if (hasUnits) { c.tuning.fuelUnit = (uint8_t)fuel; c.tuning.tripUnit = (uint8_t)trip; }
                    if (hasExp) c.exp = (uint8_t)exp;
                    cfg = c;
                }))
                return r->reply(503, "text/plain", "busy");
            if (hasUnits) _cfg->saveTuning(cfg.tuning);
            if (hasSend) _cfg->saveHuSend(send);
            if (hasExp) {
                _cfg->saveHuExp((uint8_t)exp);
                static const char* const kAt[] = {"0x21", "0x7D 04", "0x23", "?"};
                LOGI("map", "head-unit experiments: coolant %s, fuel in 7D04 %s, computed %s, selector %s, battery %s",
                               exp & Canbox::Rav4Emitter::EXP_COOLANT ? "on" : "off",
                               exp & Canbox::Rav4Emitter::EXP_FUEL_7D04 ? "on" : "off",
                               exp & Canbox::Rav4Emitter::EXP_COMPUTED ? "on" : "off",
                               exp & Canbox::Rav4Emitter::EXP_GEAR ? "on" : "off",
                               exp & Canbox::Rav4Emitter::EXP_BATT ? kAt[(exp >> 5) & 3] : "off");
            }
            if (hasSend) LOGI("map", "send to the head unit %s", send ? "on" : "off");
        }

        if (reset || n || hasConf) {
            // NVS: the newest table, packed under the staging mutex (the car
            // task only try-takes it), written after. Factory alone = no blob.
            const char* why = nullptr;
            if (!n && !hasConf) {
                if (!_cfg->eraseDecode()) why = "nvs_failed";
            } else {
                uint8_t* blob = (uint8_t*)malloc(Vehicle::DecodeTable::BLOB_MAX);
                size_t len = 0;
                if (!blob) why = "no_memory";
                else if (!App::Car::decodeTable().read([&](const Vehicle::DecodeTable& t) { len = t.pack(blob); }))
                    why = "busy";
                else if (!_cfg->saveDecode(blob, len)) why = "nvs_failed";
                free(blob);
            }
            if (hasConf)
                LOGI("map", "%s %s", Vehicle::meta(confSig).name,
                               confOn ? "confirmed in the car (measured)" : "back to a hypothesis");
            if (reset || n)
                LOGI("map", "%s: %u field(s)%s%s", reset ? "back to factory" : "saved", (unsigned)n,
                               why ? ", NOT in NVS: " : "", why ? why : "");
            else if (why)
                LOGI("map", "NOT in NVS: %s", why);
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
        const int ti = RT::find(id);
        if (ti < 0) return bad(r, 404, nullptr, "not_found");
        const TargetView t = RT::at((uint8_t)ti);
        if (!t.fw) return bad(r, 409, id, "no_field_112");
        if (!_cfg->s().uartEnabled) return r->reply(409, "text/plain", "uart_off");
        App::Hu::EmitterCfg cfg;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy");
        if (const char* why = formWhy(t.msg, cfg.tuning.cmd28)) return bad(r, 409, id, why);
        // An unknown frame to the tablet: only with its switch on, test or not.
        if (t.msg == Canbox::Rav4Emitter::MSG_GEAR && !(cfg.exp & Canbox::Rav4Emitter::EXP_GEAR))
            return bad(r, 409, id, "exp_gear_off");   // switch on the selector experiment first

        int32_t v = 0;
        if (!d["value"].is<double>() || !RT::toInt(d["value"].as<double>(), t.dec, v) ||
            v < t.min || v > t.max || (t.boolean && v != 0 && v != 1)) {
            char lo[16], hi[16], w[48];
            snprintf(w, sizeof(w), "value: between %s %s", RT::fmt(lo, sizeof(lo), t.min, t.dec),
                     RT::fmt(hi, sizeof(hi), t.max, t.dec));
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

        char hex[3 * Canbox::MAX_FRAME + 1];
        hex[0] = '\0';
        uint8_t frame[Canbox::MAX_FRAME], len = 0;
        if (App::Hu::preview(t.msg, frame, len))
            for (unsigned i = 0, o = 0; i < len; ++i) o += (unsigned)snprintf(hex + o, sizeof(hex) - o, i ? " %02X" : "%02X", frame[i]);
        char vs[16];
        LOGI("map", "test %s = %s -> %s", id, RT::fmt(vs, sizeof(vs), v, t.dec), hex);
        char b[sizeof(hex) + 24];
        snprintf(b, sizeof(b), "{\"ok\":true,\"bytes\":\"%s\"}", hex);
        return r->reply(200, "application/json", b);
    });
}

// ---------------------------------------------------------------------------
// The map in the settings file (GET/POST /api/config, WebUi.cpp)
// ---------------------------------------------------------------------------
// `decode`: the decode table exactly as NVS keeps it (DecodeTable::pack: the
// bindings, the constants, the owner's ✓), base64. `hu`: the head-unit
// emitter, as POST /api/map takes it. A file without them leaves both alone.

void WebUi::mapToFile(ChunkOut& out)
{
    uint8_t* blob = (uint8_t*)heap_caps_malloc_prefer(Vehicle::DecodeTable::BLOB_MAX, 2,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t b64cap = 4 * ((Vehicle::DecodeTable::BLOB_MAX + 2) / 3) + 1;
    char* b64 = blob ? (char*)heap_caps_malloc_prefer(b64cap, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                     : nullptr;
    size_t len = 0, olen = 0;
    if (b64 && App::Car::decodeTable().read([&](const Vehicle::DecodeTable& t) { len = t.pack(blob); }) &&
        mbedtls_base64_encode((unsigned char*)b64, b64cap, &olen, blob, len) == 0) {
        out.put(",\"decode\":\"");
        out.put(b64, olen);
        out.put("\"");
    }
    heap_caps_free(b64);
    heap_caps_free(blob);

    App::Hu::EmitterCfg c;
    if (App::Hu::emitterCfg().peek(c)) {
        char b[120];
        out.put(b, fit(snprintf(b, sizeof(b), ",\"hu\":{\"send\":%s,\"units\":{\"fuel\":%u,\"trip\":%u},\"exp\":%u}",
                                c.enabled ? "true" : "false", (unsigned)c.tuning.fuelUnit,
                                (unsigned)c.tuning.tripUnit, (unsigned)c.exp), sizeof(b)));
    }
}

const char* WebUi::mapFromFile(JsonVariantConst decode, JsonVariantConst hu, bool dry, bool& changedMap,
                               bool& changedHu)
{
    changedMap = changedHu = false;
    // Static: 2.4 kB each, and only the config POST uses them (httpd is one task).
    static Vehicle::DecodeTable table;
    static uint8_t blob[Vehicle::DecodeTable::BLOB_MAX];
    size_t len = 0;
    if (!decode.isNull()) {
        const char* s = decode.as<const char*>();
        if (!s) return "decode: not_text";
        if (mbedtls_base64_decode(blob, sizeof(blob), &len, (const unsigned char*)s, strlen(s)) != 0)
            return "decode: bad_base64";
        if (!table.unpack(blob, len)) return "decode: bad_table";
        changedMap = true;
    }

    App::Hu::EmitterCfg c;
    if (!hu.isNull()) {
        if (!hu.is<JsonObjectConst>()) return "hu: not_object";
        if (!App::Hu::emitterCfg().peek(c)) return "busy";
        if (hu["send"].is<bool>()) c.enabled = hu["send"].as<bool>() && _cfg->s().uartEnabled;
        const int fuel = hu["units"]["fuel"] | (int)c.tuning.fuelUnit;
        const int trip = hu["units"]["trip"] | (int)c.tuning.tripUnit;
        bool fok = false, tok = false;
        for (const auto& u : Canbox::FUEL_UNITS) fok |= u.v == fuel;
        for (const auto& u : Canbox::TRIP_UNITS) tok |= u.v == trip;
        if (!fok) return "hu: units.fuel";
        if (!tok) return "hu: units.trip";
        c.tuning.fuelUnit = (uint8_t)fuel;
        c.tuning.tripUnit = (uint8_t)trip;
        const int exp = hu["exp"] | (int)c.exp;
        if (exp < 0 || exp > Canbox::Rav4Emitter::EXP_ALL) return "hu: exp";
        if (const char* why = Canbox::Rav4Emitter::experimentsWhy((uint8_t)exp)) return why;
        c.exp = (uint8_t)exp;
        changedHu = true;
    }
    if (dry) return nullptr;

    if (changedMap) {
        if (!App::Car::decodeTable().edit([&](Vehicle::DecodeTable& t) { t = table; })) return "busy";
        if (!_cfg->saveDecode(blob, len)) return "nvs_failed";
        LOGI("map", "set from a settings file: %u bytes", (unsigned)len);
    }
    if (changedHu) {
        if (!App::Hu::emitterCfg().edit([&](App::Hu::EmitterCfg& e) { e = c; })) return "busy";
        _cfg->saveTuning(c.tuning);
        _cfg->saveHuSend(c.enabled);
        _cfg->saveHuExp(c.exp);
        LOGI("map", "head unit set from a settings file: send %s, exp %u", c.enabled ? "on" : "off",
                       (unsigned)c.exp);
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
