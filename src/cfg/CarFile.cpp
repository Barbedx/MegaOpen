#include "CarFile.h"
#include <initializer_list>
#include <stdio.h>
#include <string.h>
#include "../vehicle/Signals.h"

namespace CarFile
{
    namespace
    {
        const char* nameOf(uint8_t sig) { return sig < Vehicle::SIG_COUNT ? Vehicle::meta(sig).name : ""; }
        uint8_t     sigOf(JsonVariantConst v) { return Vehicle::sigByName(v | ""); }

    }

    void putDecode(JsonObject car, const Vehicle::DecodeTable& t)
    {
        JsonObject d = car["decode"].to<JsonObject>();
        JsonArray rows = d["rows"].to<JsonArray>();
        char b[256];
        for (uint16_t i = 0; i < t.row.size(); ++i) {
            const Vehicle::DecodeRow& r = t.row[i];
            JsonObject o = rows.add<JsonObject>();
            o["sig"] = nameOf(r.sig);
            Vehicle::DecodeJson::canJson(b, sizeof(b), r);
            JsonDocument can;
            deserializeJson(can, b);
            // In the file, a key at the value canFrom() takes for a missing
            // one is left out: half the bytes, the same row back.
            JsonObject c = can.as<JsonObject>();
            for (const char* k : {"le", "signed", "hyp", "latch", "key"}) if (!(c[k] | false)) c.remove(k);
            for (const char* k : {"shift", "add", "state2", "holdMs"}) if ((c[k] | 0) == 0) c.remove(k);
            for (const char* k : {"mul", "div"}) if ((c[k] | 1) == 1) c.remove(k);
            if ((c["len"] | 1) == 1) c.remove("len");
            o["can"] = c;
        }
        JsonArray ks = d["consts"].to<JsonArray>();
        for (uint16_t i = 0; i < t.k.size(); ++i) {
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
            Vehicle::DecodeRow r{};
            if (const char* why = Vehicle::DecodeJson::canFrom(o["can"].as<JsonObjectConst>(), r)) {
                t = Vehicle::DecodeTable{};
                return why;
            }
            r.sig = sig;
            // Appended unsorted, sorted once at the end.
            if (t.row.size() >= Vehicle::DecodeTable::MAX_ROWS || !t.row.push(r)) {
                t = Vehicle::DecodeTable{};
                return Vehicle::DecodeTable::WHY_ROWS;
            }
        }
        for (JsonObjectConst o : d["consts"].as<JsonArrayConst>()) {
            const uint8_t sig = sigOf(o["sig"]);
            if (sig >= Vehicle::SIG_COUNT) { ++skipped; continue; }
            if (!o["value"].is<int32_t>()) { t = Vehicle::DecodeTable{}; return "decode.consts.value: param"; }
            if (!t.addConst(sig, o["value"].as<int32_t>())) { t = Vehicle::DecodeTable{}; return Vehicle::DecodeTable::WHY_CONSTS; }
        }
        t.sortRows();
        return nullptr;
    }

    void formulaRowTo(JsonObject o, const Vehicle::FormulaRow& r)
    {
        o["name"] = r.name;
        o["f"]    = r.src;
        o["unit"] = r.unit;
        o["dec"]  = r.decimals;
        o["bool"] = r.boolean;
        o["on"]   = r.on;
    }

    const char* formulaRowFrom(JsonObjectConst o, Vehicle::FormulaRow& r)
    {
        r = Vehicle::FormulaRow{};
        const char* name = o["name"] | "";
        const char* f = o["f"] | "";
        const char* unit = o["unit"] | "";
        if (strlen(name) > Vehicle::USER_NAME) return "name: formula_name";
        if (strlen(f) > Expr::Program::MAX_SRC) return "f: expr_long";
        if (strlen(unit) > Vehicle::USER_UNIT) return "unit: param";
        const int dec = o["dec"] | 0;
        if (dec < 0 || dec > 3) return "dec: param";
        strncpy(r.name, name, Vehicle::USER_NAME);
        strncpy(r.src, f, Expr::Program::MAX_SRC);
        strncpy(r.unit, unit, Vehicle::USER_UNIT);
        r.decimals = (uint8_t)dec;
        r.boolean = o["bool"] | false;
        r.on = o["on"] | true;
        return nullptr;
    }

    void putFormulas(JsonObject car, const Vehicle::FormulaSet& set)
    {
        JsonArray a = car["formulas"].to<JsonArray>();
        for (uint16_t i = 0; i < set.rows.size(); ++i) formulaRowTo(a.add<JsonObject>(), set.rows[i]);
    }

    const char* getFormulas(JsonObjectConst car, Vehicle::FormulaSet& set)
    {
        set.rows.clear();
        if (!car["formulas"].is<JsonArrayConst>()) return "formulas: missing";
        for (JsonObjectConst o : car["formulas"].as<JsonArrayConst>()) {
            if (set.rows.size() >= Vehicle::FormulaSet::MAX_ROWS) { set.rows.clear(); return "formulas: formula_rows"; }
            Vehicle::FormulaRow r;
            if (const char* why = formulaRowFrom(o, r)) { set.rows.clear(); return why; }
            if (!set.rows.push(r)) { set.rows.clear(); return "no_memory"; }
        }
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
