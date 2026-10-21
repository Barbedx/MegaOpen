#include "WebUi.h"
#include <math.h>
#include "../app/CarTask.h"
#include "../app/HuTask.h"
#include "../proto/canbox/RaiseTargets.h"
#include "../vehicle/DecodeTable.h"
#include "../vehicle/Derived.h"
#include "../utils/LogBus.h"

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
    struct Edit {
        uint8_t            sig;
        uint8_t            src;               // Vehicle::MapSrc
        int32_t            k;
        Vehicle::DecodeRow row;
    };

    // The 0x28 form the emitter is set to send must be the one a field is in.
    const char* formWhy(uint8_t msg, uint8_t cmd28)
    {
        if (msg == Canbox::Rav4Emitter::MSG_TEMP && cmd28)
            return "0x28 carries the A/C form now: switch it in Service, Raise";
        if (msg == Canbox::Rav4Emitter::MSG_AC && !cmd28)
            return "0x28 carries the outside temperature now: switch it in Service, Raise";
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
    // [value, ageMs, src, verified]: value null when there is none, age null
    // when CAN never set it; src c=can k=const p=pinned d=derived o=off.
    // Compact on purpose: the live page polls it every 1.5 s.
    void putLive(ChunkOut& out, const Vehicle::State& st, uint32_t now)
    {
        uint8_t how[Vehicle::SIG_COUNT];
        for (uint8_t s = 0; s < Vehicle::SIG_COUNT; ++s) {
            const uint8_t* in = nullptr;
            how[s] = Vehicle::Derived::inputs(s, in) ? 'd' : 'o';
        }
        App::Car::decodeTable().read([&](const Vehicle::DecodeTable& t) {
            for (uint8_t i = 0; i < t.nRows; ++i) if (t.row[i].sig < Vehicle::SIG_COUNT) how[t.row[i].sig] = 'c';
            for (uint8_t i = 0; i < t.nK; ++i)    if (t.k[i].sig < Vehicle::SIG_COUNT) how[t.k[i].sig] = 'k';
        });
        char b[64], v[16], a[12];
        out.put(",\"sigs\":[");
        for (uint8_t s = 0; s < Vehicle::SIG_COUNT; ++s) {
            const char h = st.pinned(s) && how[s] != 'k' ? 'p' : (char)how[s];
            const uint32_t age = st.ageMs(s, now);
            if (age == Vehicle::State::NEVER) snprintf(a, sizeof(a), "null");
            else snprintf(a, sizeof(a), "%lu", (unsigned long)age);
            out.put(b, fit(snprintf(b, sizeof(b), "%s[%s,%s,\"%c\",%d]", s ? "," : "",
                                    st.has(s) ? dec(v, sizeof(v), st.get(s), Vehicle::meta(s).decimals) : "null",
                                    a, h, Vehicle::verified(s) ? 1 : 0), sizeof(b)));
        }
        const App::Car::AlertStats as = App::Car::alertStats();
        out.put(b, fit(snprintf(b, sizeof(b), "],\"alertStats\":{\"fired\":%lu,\"shown\":%lu,\"dropped\":%lu}",
                                (unsigned long)as.fired, (unsigned long)as.shown, (unsigned long)as.dropped), sizeof(b)));
    }

    // ?meta=1: what the live page needs once — every signal's name, unit,
    // decimals, yes/no, group, range and Ukrainian label ("" = the Raise
    // field of that name has one). The alert rules are a settings-registry
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
                                    Vehicle::label(s)),
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
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy, try again");
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
    // {reset:true} (back to decode_defaults.inc: the NVS blob is erased). Every
    // part is checked before any is applied. A row replaces whatever bound
    // that field; the map goes to the car task (no reboot) and to NVS (`decode`).
    route("/api/map", HTTP_POST, [this](PsychicRequest* r) {
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad json");

        App::Hu::EmitterCfg cfg;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy, try again");

        // ---- check ----
        const bool hasSend = d["send"].is<bool>();
        const bool send = d["send"] | false;
        if (hasSend && send && !_cfg->s().uartEnabled) return r->reply(409, "text/plain", "head-unit UART is off");

        // {exp: n}: the head-unit experiments, Rav4Emitter::Exp bits.
        const bool hasExp = !d["exp"].isNull();
        const int exp = d["exp"] | -1;
        if (hasExp && (!d["exp"].is<int>() || exp < 0 || exp > Canbox::Rav4Emitter::EXP_ALL))
            return r->reply(400, "text/plain", "exp: 0..7, the experiment bits");

        const bool hasUnits = d["units"].is<JsonObject>();
        int fuel = cfg.tuning.fuelUnit, trip = cfg.tuning.tripUnit;
        if (hasUnits) {
            fuel = d["units"]["fuel"] | fuel;
            trip = d["units"]["trip"] | trip;
            bool fok = false, tok = false;
            for (const auto& u : Canbox::FUEL_UNITS) fok |= u.v == fuel;
            for (const auto& u : Canbox::TRIP_UNITS) tok |= u.v == trip;
            if (!fok) return r->reply(400, "text/plain", "units.fuel: one of the catalog's fuelUnits");
            if (!tok) return r->reply(400, "text/plain", "units.trip: one of the catalog's tripUnits");
        }

        const bool reset = d["reset"] | false;
        Edit ed[MAX_EDITS];
        uint8_t n = 0;
        JsonArrayConst rows = d["rows"].as<JsonArrayConst>();
        if (rows.size() > MAX_EDITS) return r->reply(400, "text/plain", "rows: 16 at most in one request");
        for (JsonObjectConst o : rows) {
            const char* id = o["id"] | "";
            const int ti = RT::find(id);
            if (ti < 0) return bad(r, 404, nullptr, "no such field");
            const TargetView t = RT::at((uint8_t)ti);
            if (t.sig >= Vehicle::SIG_COUNT) return bad(r, 409, id, "the firmware has no signal for it yet");
            const int s = uiFind(Vehicle::MAP_SRC, o["src"] | "");
            if (s < 0) return bad(r, 400, id, "src: can, const or off");
            for (uint8_t j = 0; j < n; ++j)
                if (ed[j].sig == t.sig) return bad(r, 400, id, "twice in one request");
            Edit& e = ed[n++];
            e.sig = t.sig;
            e.src = (uint8_t)s;
            e.k = 0;
            e.row = Vehicle::DecodeRow{};
            if (s == Vehicle::SRC_CAN) {
                if (!o["can"].is<JsonObjectConst>()) return bad(r, 400, id, "can: {id, byte, len, ...}");
                if (const char* why = Vehicle::DecodeJson::canFrom(o["can"], e.row)) return bad(r, 400, id, why);
            } else if (s == Vehicle::SRC_CONST) {
                if (!o["k"].is<double>() || !RT::toInt(o["k"].as<double>(), t.dec, e.k) ||
                    e.k < t.min || e.k > t.max) {
                    char lo[16], hi[16], w[48];
                    snprintf(w, sizeof(w), "k: %s..%s", RT::fmt(lo, sizeof(lo), t.min, t.dec),
                             RT::fmt(hi, sizeof(hi), t.max, t.dec));
                    return bad(r, 400, id, w);
                }
            }
        }

        // ---- apply ----
        // The map first: its room check is the last refusal, so a refused
        // request has changed nothing.
        if (reset || n) {
            const char* full = nullptr;
            bool bound = true;
            if (!App::Car::decodeTable().edit([&](Vehicle::DecodeTable& t) {
                    // Reset first is safe: factory (62) plus 16 edits always fits.
                    if (reset) t.factory();
                    // Room first: a table half-changed by a refused request
                    // would be worse than the refusal.
                    uint8_t rowsLeft = t.nRows, constsLeft = t.nK;
                    for (uint8_t i = 0; i < n; ++i) {
                        for (uint8_t j = 0; j < t.nRows; ++j) if (t.row[j].sig == ed[i].sig) --rowsLeft;
                        if (t.constOf(ed[i].sig) >= 0) --constsLeft;
                    }
                    for (uint8_t i = 0; i < n; ++i) {
                        if (ed[i].src == Vehicle::SRC_CAN)   ++rowsLeft;
                        if (ed[i].src == Vehicle::SRC_CONST) ++constsLeft;
                    }
                    if (rowsLeft > Vehicle::DecodeTable::MAX_ROWS) { full = "the map is full: 96 CAN rows"; return; }
                    if (constsLeft > Vehicle::DecodeTable::MAX_CONSTS) { full = "the map is full: 16 constants"; return; }
                    for (uint8_t i = 0; i < n; ++i) {
                        const Edit& e = ed[i];
                        if (e.src == Vehicle::SRC_CAN) {
                            // A latch or a key flag is the row's own behaviour,
                            // which the page does not edit: kept from the old row.
                            const int old = t.rowOf(e.sig);
                            Vehicle::DecodeRow row = e.row;
                            if (old >= 0) {
                                row.flags |= t.row[old].flags & (Vehicle::DF_LATCH | Vehicle::DF_KEY);
                                if (t.row[old].flags & Vehicle::DF_LATCH) row.holdMs = t.row[old].holdMs;
                                // Re-saving the same field is not a measurement:
                                // a hypothesis stays one, and a 2-bit state keeps
                                // its reading. A changed field is the user's own,
                                // so it counts as measured and reads plain.
                                if (Vehicle::DecodeTable::sameField(row, t.row[old]))
                                    row.flags |= t.row[old].flags & (Vehicle::DF_HYPOTHESIS | Vehicle::DF_STATE2 | Vehicle::DF_STATE2_INV);
                            }
                            bound &= t.bindCan(e.sig, row);
                        } else if (e.src == Vehicle::SRC_CONST) {
                            bound &= t.bindConst(e.sig, e.k);
                        } else {
                            t.unbind(e.sig);
                        }
                    }
                }))
                return r->reply(503, "text/plain", "busy, try again");
            if (full) return r->reply(409, "text/plain", full);
            // The room check above makes this unreachable; never a quiet 200.
            if (!bound) return r->reply(500, "text/plain", "the map refused a binding: not saved");
        }

        if (hasSend || hasUnits || hasExp) {
            if (!App::Hu::emitterCfg().edit([&](App::Hu::EmitterCfg& c) {
                    if (hasSend) c.enabled = send;
                    if (hasUnits) { c.tuning.fuelUnit = (uint8_t)fuel; c.tuning.tripUnit = (uint8_t)trip; }
                    if (hasExp) c.exp = (uint8_t)exp;
                    cfg = c;
                }))
                return r->reply(503, "text/plain", "busy, try again");
            if (hasUnits) _cfg->saveTuning(cfg.tuning);
            if (hasSend) _cfg->saveHuSend(send);
            if (hasExp) {
                _cfg->saveHuExp((uint8_t)exp);
                LogBus::printf("[map] head-unit experiments: coolant %s, fuel in 7D04 %s, computed %s",
                               exp & Canbox::Rav4Emitter::EXP_COOLANT ? "on" : "off",
                               exp & Canbox::Rav4Emitter::EXP_FUEL_7D04 ? "on" : "off",
                               exp & Canbox::Rav4Emitter::EXP_COMPUTED ? "on" : "off");
            }
            if (hasSend) LogBus::printf("[map] send to the head unit %s", send ? "on" : "off");
        }

        if (reset || n) {
            // NVS: the newest table, packed under the staging mutex (the car
            // task only try-takes it), written after. Factory alone = no blob.
            const char* why = nullptr;
            if (!n) {
                if (!_cfg->eraseDecode()) why = "NVS erase failed";
            } else {
                uint8_t* blob = (uint8_t*)malloc(Vehicle::DecodeTable::BLOB_MAX);
                size_t len = 0;
                if (!blob) why = "no memory";
                else if (!App::Car::decodeTable().read([&](const Vehicle::DecodeTable& t) { len = t.pack(blob); }))
                    why = "busy";
                else if (!_cfg->saveDecode(blob, len)) why = "NVS write failed";
                free(blob);
            }
            LogBus::printf("[map] %s: %u field(s)%s%s", reset ? "back to factory" : "saved", (unsigned)n,
                           why ? ", NOT in NVS: " : "", why ? why : "");
            if (why) return bad(r, 500, "applied until the next boot only", why);
        }

        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // {id, value}: the value is pinned for 30 s (RAM only, a pin wins over
    // CAN) and the field's message goes to the head unit now, once. With
    // `send` on it keeps going out on its schedule until the 30 s are up.
    route("/api/map/test", HTTP_POST, [this](PsychicRequest* r) {
        if (!_state) return r->reply(503, "text/plain", "no vehicle");
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad json");
        const char* id = d["id"] | "";
        const int ti = RT::find(id);
        if (ti < 0) return bad(r, 404, nullptr, "no such field");
        const TargetView t = RT::at((uint8_t)ti);
        if (!t.fw) {
            char w[64];
            snprintf(w, sizeof(w), "firmware does not send %s yet", t.cmd);
            return bad(r, 409, id, w);
        }
        if (!_cfg->s().uartEnabled) return r->reply(409, "text/plain", "head-unit UART is off");
        App::Hu::EmitterCfg cfg;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy, try again");
        if (const char* why = formWhy(t.msg, cfg.tuning.cmd28)) return bad(r, 409, id, why);

        int32_t v = 0;
        if (!d["value"].is<double>() || !RT::toInt(d["value"].as<double>(), t.dec, v) ||
            v < t.min || v > t.max || (t.boolean && v != 0 && v != 1)) {
            char lo[16], hi[16], w[48];
            snprintf(w, sizeof(w), "value: %s..%s", RT::fmt(lo, sizeof(lo), t.min, t.dec),
                     RT::fmt(hi, sizeof(hi), t.max, t.dec));
            return bad(r, 400, id, w);
        }

        // The car task pins it; the message is built from State, so wait
        // until the pin is there.
        App::Car::test(t.sig, v, TEST_MS);
        const uint32_t t0 = millis();
        while (!(_state->pinned(t.sig) && _state->get(t.sig) == v)) {
            if ((uint32_t)(millis() - t0) > APPLY_WAIT_MS)
                return r->reply(503, "text/plain", "car task busy, try again");
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        switch (App::Hu::sendNowWait(t.msg)) {
            case App::Hu::SendNow::Sent: break;
            // Trips ride on the odometer's 0x7D 04, tyres on "TPMS є", hybrid on
            // "Гібрид є", the rear radar on "Радар увімкнено": the message goes
            // only once its gate has a value.
            case App::Hu::SendNow::NoData:
                return bad(r, 409, id, "its message needs its main field too (odometer, radar on, TPMS, hybrid): test that first");
            default: return r->reply(503, "text/plain", "uart task busy, try again");
        }

        char hex[3 * Canbox::MAX_FRAME + 1];
        hex[0] = '\0';
        uint8_t frame[Canbox::MAX_FRAME], len = 0;
        if (App::Hu::preview(t.msg, frame, len))
            for (unsigned i = 0, o = 0; i < len; ++i) o += (unsigned)snprintf(hex + o, sizeof(hex) - o, i ? " %02X" : "%02X", frame[i]);
        char vs[16];
        LogBus::printf("[map] test %s = %s -> %s", id, RT::fmt(vs, sizeof(vs), v, t.dec), hex);
        char b[sizeof(hex) + 24];
        snprintf(b, sizeof(b), "{\"ok\":true,\"bytes\":\"%s\"}", hex);
        return r->reply(200, "application/json", b);
    });
}
