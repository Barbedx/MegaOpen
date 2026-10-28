#include "CarFile.h"
#include <stdio.h>
#include <string.h>
#include "../vehicle/Signals.h"

namespace CarFile
{
    namespace
    {
        const char* nameOf(uint8_t sig) { return sig < Vehicle::SIG_COUNT ? Vehicle::meta(sig).name : ""; }
        uint8_t     sigOf(JsonVariantConst v) { return Vehicle::sigByName(v | ""); }

        // Rows stay sorted by id (CanDecode::load needs it), equal ids in
        // file order.
        void sortRows(Vehicle::DecodeTable& t)
        {
            for (uint8_t i = 1; i < t.nRows; ++i) {
                const Vehicle::DecodeRow r = t.row[i];
                uint8_t j = i;
                while (j && t.row[j - 1].canId > r.canId) { t.row[j] = t.row[j - 1]; --j; }
                t.row[j] = r;
            }
        }
    }

    void putDecode(JsonObject car, const Vehicle::DecodeTable& t)
    {
        JsonObject d = car["decode"].to<JsonObject>();
        JsonArray rows = d["rows"].to<JsonArray>();
        char b[256];
        for (uint8_t i = 0; i < t.nRows; ++i) {
            const Vehicle::DecodeRow& r = t.row[i];
            JsonObject o = rows.add<JsonObject>();
            o["sig"] = nameOf(r.sig);
            Vehicle::DecodeJson::canJson(b, sizeof(b), r);
            JsonDocument can;
            deserializeJson(can, b);
            o["can"] = can.as<JsonObjectConst>();
        }
        JsonArray ks = d["consts"].to<JsonArray>();
        for (uint8_t i = 0; i < t.nK; ++i) {
            JsonObject o = ks.add<JsonObject>();
            o["sig"]   = nameOf(t.k[i].sig);
            o["value"] = t.k[i].value;
        }
    }

    const char* getDecode(JsonObjectConst car, Vehicle::DecodeTable& t, uint16_t& skipped)
    {
        t = Vehicle::DecodeTable{};
        skipped = 0;
        JsonObjectConst d = car["decode"].as<JsonObjectConst>();
        if (d.isNull()) return "decode: missing";
        for (JsonObjectConst o : d["rows"].as<JsonArrayConst>()) {
            const uint8_t sig = sigOf(o["sig"]);
            if (sig >= Vehicle::SIG_COUNT) { ++skipped; continue; }
            if (t.nRows >= Vehicle::DecodeTable::MAX_ROWS) { t = Vehicle::DecodeTable{}; return Vehicle::DecodeTable::WHY_ROWS; }
            Vehicle::DecodeRow r{};
            if (const char* why = Vehicle::DecodeJson::canFrom(o["can"].as<JsonObjectConst>(), r)) {
                t = Vehicle::DecodeTable{};
                return why;
            }
            r.sig = sig;
            t.row[t.nRows++] = r;
        }
        for (JsonObjectConst o : d["consts"].as<JsonArrayConst>()) {
            const uint8_t sig = sigOf(o["sig"]);
            if (sig >= Vehicle::SIG_COUNT) { ++skipped; continue; }
            if (!o["value"].is<int32_t>()) { t = Vehicle::DecodeTable{}; return "decode.consts.value: param"; }
            if (t.nK >= Vehicle::DecodeTable::MAX_CONSTS) { t = Vehicle::DecodeTable{}; return Vehicle::DecodeTable::WHY_CONSTS; }
            t.k[t.nK++] = Vehicle::DecodeConst{sig, o["value"].as<int32_t>()};
        }
        sortRows(t);
        return nullptr;
    }

    int ruleJson(char* b, size_t cap, const Canbox::FieldRule& r)
    {
        static const char* const FROM[] = {"", "sig", "k", "off"};
        const bool own = r.kind == Canbox::FK_SIGNAL && r.src == r.field;
        const int n = snprintf(b, cap, "{\"from\":\"%s\",\"src\":\"%s\",\"mul\":%d,\"div\":%d,\"add\":%ld}",
                               own ? "own" : FROM[r.kind <= 3 ? r.kind : 0],
                               r.kind == Canbox::FK_SIGNAL ? nameOf(r.src) : "",
                               (int)r.mul, (int)r.div, (long)r.add);
        return n < 0 ? 0 : (size_t)n < cap ? n : (int)cap - 1;
    }

    const char* ruleFrom(JsonObjectConst o, uint8_t field, Canbox::FieldRule& r, bool& clear)
    {
        clear = false;
        r = Canbox::FieldRule{};
        r.field = field;
        const char* from = o["from"] | "auto";
        if (!strcmp(from, "auto")) { clear = true; return nullptr; }
        if (!strcmp(from, "off")) { r.kind = Canbox::FK_OFF; return nullptr; }
        if (!strcmp(from, "k"))   { r.kind = Canbox::FK_CONST; r.add = o["add"] | 0; return nullptr; }
        if (!strcmp(from, "own")) { r.kind = Canbox::FK_SIGNAL; r.src = field; return nullptr; }
        if (strcmp(from, "sig")) return "from: param";
        r.kind = Canbox::FK_SIGNAL;
        r.src = sigOf(o["src"]);
        if (r.src >= Vehicle::SIG_COUNT) return "src: param";
        r.mul = (int16_t)(o["mul"] | 1);
        r.div = (int16_t)(o["div"] | 1);
        r.add = o["add"] | 0;
        if (!r.div) return "div: param";
        return nullptr;
    }

    void putFields(JsonObject car, const Canbox::Protocol& p, const Canbox::FieldMap& m)
    {
        JsonObject hu = car["hu"].is<JsonObject>() ? car["hu"].as<JsonObject>() : car["hu"].to<JsonObject>();
        JsonArray fs = hu[p.id()].to<JsonObject>()["fields"].to<JsonArray>();
        char b[160];
        for (uint8_t i = 0; i < m.count(); ++i) {
            const Canbox::FieldRule& r = m.at(i);
            ruleJson(b, sizeof(b), r);
            JsonDocument one;
            deserializeJson(one, b);
            JsonObject o = fs.add<JsonObject>();
            o["field"] = nameOf(r.field);
            for (JsonPairConst kv : one.as<JsonObjectConst>()) o[kv.key()] = kv.value();
        }
    }

    const char* getFields(JsonObjectConst car, const Canbox::Protocol& p, Canbox::FieldMap& m, uint16_t& skipped)
    {
        m.clearAll();
        skipped = 0;
        for (JsonObjectConst o : car["hu"][p.id()]["fields"].as<JsonArrayConst>()) {
            const int fi = p.findField(o["field"] | "");
            if (fi < 0) { ++skipped; continue; }
            Canbox::FieldRule r;
            bool clear = false;
            if (const char* why = ruleFrom(o, p.field((uint8_t)fi).sig, r, clear)) {
                // A source this firmware does not know: skipped, not refused.
                if (!strcmp(why, "src: param")) { ++skipped; continue; }
                m.clearAll();
                return why;
            }
            if (clear) continue;
            if (!m.set(r)) { m.clearAll(); return "rows_max 29"; }
        }
        return nullptr;
    }
}
