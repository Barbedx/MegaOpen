#include "WebUi.h"
#include <stdio.h>
#include <string.h>
#include "../app/CarTask.h"
#include "../app/KeyBus.h"
#include "../cfg/CarFile.h"
#include "../cfg/CarStore.h"
#include "../utils/LogBus.h"
#include "../utils/PsramJson.h"
#include "../utils/Scratch.h"
#include "../vehicle/Rules.h"

// The rules (docs/design/rules.md, slice 4): «Мапа › Правила».
//   GET  /api/rules           {rows:[{...CarFile::ruleRowTo, live:{on, holds,
//                             fires, agoMs}}], problem, stats}
//   POST /api/rules           {rows:[...]} replaces the set, {reset:true} brings
//                             the factory's back. Checked first (400 "<row
//                             name>: <code> [byte]"), then staged for the car
//                             task, the gesture rows re-tabled for the keys task
//                             (MacroEngine), and saved in the car file.
//   POST /api/rules/test?id=N the row's actions now: no trigger, no pause (202).
void WebUi::routesRules()
{
    route("/api/rules", HTTP_GET, [this](PsychicRequest* r) {
        Scratch<Vehicle::RuleSet> set;
        if (!set) return r->reply(503, "text/plain", "no_memory");
        if (!App::Car::ruleSet().peek(*set)) return r->reply(503, "text/plain", "busy");
        JsonDocument d(&PsramJson::instance());
        JsonArray rows = d["rows"].to<JsonArray>();
        const uint32_t now = millis();
        for (uint16_t i = 0; i < set->rows.size(); ++i) {
            JsonObject o = rows.add<JsonObject>();
            CarFile::ruleRowTo(o, set->rows[i]);
            Vehicle::Rules::Live l;
            if (App::Car::ruleLive(i, l)) {
                JsonObject v = o["live"].to<JsonObject>();
                v["on"]    = l.running;
                v["holds"] = l.holds;
                v["fires"] = l.fires;
                if (l.fires) v["agoMs"] = now - l.lastFireMs;
                else         v["agoMs"] = nullptr;
            }
        }
        Vehicle::RuleProblem pr;
        if (Vehicle::Rules::check(*set, pr)) d["problem"] = nullptr;
        else {
            JsonObject o = d["problem"].to<JsonObject>();
            o["row"]  = pr.row;
            o["code"] = pr.e.code ? pr.e.code : "";
            o["pos"]  = pr.e.pos;
        }
        const App::Car::RuleStats st = App::Car::ruleStats();
        JsonObject s = d["stats"].to<JsonObject>();
        s["fired"]   = st.fired;
        s["shown"]   = st.shown;
        s["dropped"] = st.dropped;
        String out;
        serializeJson(d, out);
        return replyJson(r, out);
    });

    route("/api/rules", HTTP_POST, [this](PsychicRequest* r) {
        JsonDocument d(&PsramJson::instance());
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        Scratch<Vehicle::RuleSet> set;
        if (!set) return r->reply(503, "text/plain", "no_memory");
        const bool reset = d["reset"] | false;
        if (reset) {
            set->factory();
        } else {
            if (!d["rows"].is<JsonArrayConst>()) return r->reply(400, "text/plain", "param rows");
            JsonDocument wrap(&PsramJson::instance());
            wrap["rules"] = d["rows"];
            if (const char* why = CarFile::getRules(wrap.as<JsonObjectConst>(), *set))
                return r->reply(400, "text/plain", why);
        }
        Vehicle::RuleProblem pr;
        if (!Vehicle::Rules::check(*set, pr)) {
            // "<row name>: <code>", a formula's code with its byte.
            char b[128];
            const char* name = pr.row < set->rows.size() ? set->rows[pr.row].name : "";
            const char* code = pr.e.code ? pr.e.code : "?";
            if (!strncmp(code, "expr_", 5)) snprintf(b, sizeof(b), "%s%s%s %u", name, *name ? ": " : "", code, (unsigned)pr.e.pos);
            else                            snprintf(b, sizeof(b), "%s%s%s", name, *name ? ": " : "", code);
            return r->reply(400, "text/plain", b);
        }
        uint8_t gestures = 0;
        const char* why = applyRules(*set, reset, gestures);
        if (why && strcmp(why, "file_failed")) return r->reply(503, "text/plain", why);
        const bool saved = !why;
        uint16_t on = 0;
        for (uint16_t i = 0; i < set->rows.size(); ++i) on += set->rows[i].on ? 1 : 0;
        LOGI("rule", "%u rule(s), %u on, %u gesture(s)%s%s", (unsigned)set->rows.size(), (unsigned)on, (unsigned)gestures,
             reset ? ", the factory's" : "", saved ? "" : ", NOT in the car file");
        return r->reply(200, "application/json", saved ? "{\"ok\":true}" : "{\"ok\":true,\"saved\":false}");
    });

    route("/api/rules/test", HTTP_POST, [](PsychicRequest* r) {
        if (!r->hasParam("id")) return r->reply(400, "text/plain", "param id");
        const long id = atol(r->getParam("id")->value().c_str());
        if (id < 0 || id >= Vehicle::RuleSet::MAX_ROWS) return r->reply(400, "text/plain", "param id");
        App::Car::testRule((uint16_t)id);
        return r->reply(202, "application/json", "{\"ok\":true,\"queued\":true}");
    });
}

// A checked set into the car task first, then the gesture rows into the keys
// task's table (MacroEngine, Keys::Macro::rule pointing back at the row), then
// the car file. nullptr, "busy" (the car task holds its set), "file_failed"
// (applied, not saved). POST /api/rules and the settings file import.
const char* WebUi::applyRules(const Vehicle::RuleSet& set, bool factory, uint8_t& gestures)
{
    gestures = 0;
    if (!App::Car::ruleSet().edit([&](Vehicle::RuleSet& s) { s = set; })) return "busy";
    if (_st.macros) {
        const bool ok = _st.macros->edit([&](Keys::MacroTable& t) {
            gestures = Vehicle::Rules::gestures(set, t, Keys::MacroTable::COUNT);
        });
        if (ok) App::KeyBus::kick();
        else    LOGW("rule", "the gesture table is busy: the keys task keeps the old gestures until the next save");
    }
    const bool saved = factory ? CarStore::factoryRules() : CarStore::saveRules(set);
    return saved ? nullptr : "file_failed";
}
