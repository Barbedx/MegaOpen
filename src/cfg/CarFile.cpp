#include "CarFile.h"
#include "../utils/Hex.h"
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

        // A char[N] member of a const struct reaches ArduinoJson as a `const char
        // (&)[N]`, which 7.x takes for a string literal: the document keeps the
        // POINTER (StringAdapter<const char(&)[N]>, "static"), and the car
        // document (CarStore, alive for the whole boot) reads freed memory once
        // the struct is gone: 1.6.9 put the formulas in that way, 1.7.0's first
        // bench boot wrote sixteen garbage rule names. Through a plain pointer
        // the text is copied (test_carfile: the document owns its strings).
        const char* copied(const char* s) { return s; }
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
        o["name"] = copied(r.name);
        o["f"]    = copied(r.src);
        o["unit"] = copied(r.unit);
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

    // ---- the rules -----------------------------------------------------------------

    namespace
    {
        void actionTo(JsonObject o, const Vehicle::RuleAction& a)
        {
            o["effect"] = Keys::effectName(a.effect);
            char hex[4];
            switch (a.effect) {
                case Keys::Effect::Action:
                    o["action"] = Keys::name(a.action);
                    if (a.code) { snprintf(hex, sizeof(hex), "%02X", a.code); o["codeHex"] = hex; }
                    break;
                case Keys::Effect::KeyCode:
                case Keys::Effect::Message:
                    snprintf(hex, sizeof(hex), "%02X", a.code);
                    o["codeHex"] = hex;
                    break;
                case Keys::Effect::Raw: {
                    snprintf(hex, sizeof(hex), "%02X", a.code);
                    o["codeHex"] = hex;
                    char data[Keys::Macro::MAX_DATA * 3];
                    int n = 0;
                    for (uint8_t j = 0; j < a.dataLen && j < Keys::Macro::MAX_DATA; ++j)
                        n += snprintf(data + n, sizeof(data) - n, "%s%02X", j ? " " : "", a.data[j]);
                    data[n] = 0;
                    o["data"] = data;    // copied by the document
                    break;
                }
                default:
                    o["arg"] = copied(a.arg);   // Text, Popup, Pane
                    break;
            }
        }

        // "01 A0 ff" -> bytes; -1 on a bad digit or too many.
        int hexBytes(const char* s, uint8_t* out, uint8_t cap)
        {
            uint8_t n = 0;
            int hi = -1;
            for (; *s; ++s) {
                const int v = Hex::nibble(*s);
                if (v < 0) {
                    if (*s == ' ' || *s == ':' || *s == ',') { if (hi >= 0) return -1; continue; }
                    return -1;
                }
                if (hi < 0) { hi = v; continue; }
                if (n >= cap) return -1;
                out[n++] = (uint8_t)((hi << 4) | v);
                hi = -1;
            }
            return hi >= 0 ? -1 : (int)n;
        }

        const char* actionFrom(JsonObjectConst o, Vehicle::RuleAction& a)
        {
            a = Vehicle::RuleAction{};
            a.effect = Keys::effectFromName(o["effect"] | "none");
            if (a.effect == Keys::Effect::None) return "do.effect: param";
            a.action = Keys::fromName(o["action"] | "None");
            const char* hex = o["codeHex"] | "";
            if (hex[0]) {
                uint8_t c;
                if (hexBytes(hex, &c, 1) != 1) return "do.codeHex: param";
                a.code = c;
            }
            const char* data = o["data"] | "";
            if (data[0]) {
                const int n = hexBytes(data, a.data, Keys::Macro::MAX_DATA);
                if (n < 0) return "do.data: macro_hex 8";
                a.dataLen = (uint8_t)n;
            }
            const char* arg = o["arg"] | "";
            if (strlen(arg) >= Keys::Macro::ARG_LEN) return "do.arg: too_long";
            strncpy(a.arg, arg, Keys::Macro::ARG_LEN - 1);
            return nullptr;
        }
    }

    void ruleRowTo(JsonObject o, const Vehicle::RuleRow& r)
    {
        o["name"] = copied(r.name);
        o["on"]   = r.on;
        if (r.isGesture()) {
            JsonObject g = o["gesture"].to<JsonObject>();
            g["trigger"] = Keys::triggerName(r.gesture.trigger);
            JsonArray k = g["keys"].to<JsonArray>();
            const uint8_t n = r.gesture.trigger == Keys::Trigger::Combo ? r.gesture.members : 1;
            for (uint8_t j = 0; j < n && j < Keys::Macro::MAX_MEMBERS; ++j) k.add(Keys::name(r.gesture.keys[j]));
            if (r.gesture.trigger == Keys::Trigger::Sequence) {
                g["taps"]     = r.gesture.taps;
                g["windowMs"] = r.gesture.windowMs;
            }
            g["swallow"] = r.gesture.swallow;
        }
        o["when"]  = copied(r.when);
        o["pause"] = r.pauseS;
        JsonArray d = o["do"].to<JsonArray>();
        for (uint8_t k = 0; k < r.nAct && k < Vehicle::RuleRow::MAX_ACTIONS; ++k) actionTo(d.add<JsonObject>(), r.act[k]);
    }

    const char* ruleRowFrom(JsonObjectConst o, Vehicle::RuleRow& r)
    {
        r = Vehicle::RuleRow{};
        const char* name = o["name"] | "";
        const char* when = o["when"] | "";
        if (!name[0] || strlen(name) > Vehicle::RULE_NAME) return "name: rule_name";
        if (strlen(when) > Expr::Program::MAX_SRC) return "when: expr_long";
        strncpy(r.name, name, Vehicle::RULE_NAME);
        strncpy(r.when, when, Expr::Program::MAX_SRC);
        r.on = o["on"] | true;
        const long pause = o["pause"] | (long)Vehicle::RuleRow::PAUSE_MIN_S;
        if (pause < 0 || pause > 65535) return "pause: between 0 65535";
        r.pauseS = (uint16_t)pause;
        if (o["gesture"].is<JsonObjectConst>()) {
            JsonObjectConst g = o["gesture"];
            r.gesture.trigger = Keys::triggerFromName(g["trigger"] | "off");
            if (r.gesture.trigger == Keys::Trigger::Off) return "gesture.trigger: param";
            r.gesture.members = 0;
            for (JsonVariantConst v : g["keys"].as<JsonArrayConst>()) {
                if (r.gesture.members >= Keys::Macro::MAX_MEMBERS) break;
                const Keys::Action a = Keys::fromName(v | "None");
                if (a != Keys::Action::None) r.gesture.keys[r.gesture.members++] = a;
            }
            const long taps = g["taps"] | 3L, win = g["windowMs"] | 600L;
            if (taps < 2 || taps > 10) return "gesture.taps: between 2 10";
            if (win < 50 || win > 65535) return "gesture.windowMs: between 50 65535";
            r.gesture.taps = (uint8_t)taps;
            r.gesture.windowMs = (uint16_t)win;
            r.gesture.swallow = g["swallow"] | true;
        }
        r.nAct = 0;
        for (JsonObjectConst a : o["do"].as<JsonArrayConst>()) {
            if (r.nAct >= Vehicle::RuleRow::MAX_ACTIONS) return "do: rows_max 3";
            if (const char* why = actionFrom(a, r.act[r.nAct])) return why;
            ++r.nAct;
        }
        return nullptr;
    }

    void putRules(JsonObject car, const Vehicle::RuleSet& set)
    {
        JsonArray a = car["rules"].to<JsonArray>();
        for (uint16_t i = 0; i < set.rows.size(); ++i) ruleRowTo(a.add<JsonObject>(), set.rows[i]);
    }

    const char* getRules(JsonObjectConst car, Vehicle::RuleSet& set)
    {
        set.rows.clear();
        if (!car["rules"].is<JsonArrayConst>()) return "rules: missing";
        for (JsonObjectConst o : car["rules"].as<JsonArrayConst>()) {
            if (set.rows.size() >= Vehicle::RuleSet::MAX_ROWS) { set.rows.clear(); return "rules: rule_rows"; }
            Vehicle::RuleRow r;
            if (const char* why = ruleRowFrom(o, r)) { set.rows.clear(); return why; }
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
