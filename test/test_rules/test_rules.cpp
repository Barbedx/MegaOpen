#include <unity.h>
#include <string.h>
#include <string>
#include <ArduinoJson.h>
#include "vehicle/Rules.h"
#include "vehicle/Signals.h"
#include "cfg/CarFile.h"

void setUp() {}
void tearDown() {}

// Rules (src/vehicle/Rules.h): the signal rules that took over the panel
// alerts, the gesture rules that took over the macros, and 1.6's tables
// carried into them.
using namespace Vehicle;

namespace
{
    uint16_t g_fired[32];
    uint8_t  g_nFired = 0;
    void onFire(uint16_t row, void*) { if (g_nFired < 32) g_fired[g_nFired++] = row; }

    // One pass: what changed in State, through the rules.
    void pass(Rules& r, State& s, uint32_t now, bool tick = false)
    {
        Expr::SigSet ch;
        s.takeChanged(ch);
        r.step(s, ch, now, tick, onFire, nullptr);
    }
    void fresh() { g_nFired = 0; }

    RuleRow signalRule(const char* name, const char* when, uint16_t pause = 5)
    {
        RuleRow r;
        strncpy(r.name, name, RULE_NAME);
        strncpy(r.when, when, Expr::Program::MAX_SRC);
        r.pauseS = pause;
        r.act[0].effect = Keys::Effect::Popup;
        r.nAct = 1;
        return r;
    }
    RuleRow gestureRule(const char* name, Keys::Trigger t, Keys::Action a, Keys::Action b = Keys::Action::None)
    {
        RuleRow r;
        strncpy(r.name, name, RULE_NAME);
        r.gesture.trigger = t;
        r.gesture.keys[0] = a;
        r.gesture.keys[1] = b;
        r.gesture.members = b != Keys::Action::None ? 2 : 1;
        r.gesture.taps = 3;
        r.pauseS = 0;
        r.act[0].effect = Keys::Effect::KeyCode;
        r.act[0].code = 0x07;
        r.nAct = 1;
        return r;
    }
}

// The three alerts 1.6 shipped are three rules; the low-fuel lamp fires once
// on its rising edge, then not again until the pause has run out.
void test_the_factory_rules_are_the_old_alerts()
{
    RuleSet set;
    set.factory();
    TEST_ASSERT_EQUAL_UINT16(3, set.rows.size());
    TEST_ASSERT_EQUAL_STRING("fuel_low", set.rows[0].name);
    TEST_ASSERT_EQUAL_STRING("rises(fuel_low)", set.rows[0].when);
    TEST_ASSERT_EQUAL_UINT16(300, set.rows[0].pauseS);
    TEST_ASSERT_EQUAL_STRING("drive_no_lights", set.rows[1].name);
    TEST_ASSERT_EQUAL_STRING("Lights off!", set.rows[1].act[0].arg);       // the reminders' words stay
    TEST_ASSERT_EQUAL_STRING("drive_handbrake", set.rows[2].name);
    TEST_ASSERT_EQUAL_STRING("Handbrake on!", set.rows[2].act[0].arg);
    TEST_ASSERT_EQUAL_STRING("", set.rows[0].act[0].arg);                  // the default text
    RuleProblem p;
    TEST_ASSERT_TRUE(Rules::check(set, p));
    Rules r;
    TEST_ASSERT_TRUE(r.apply(set, p));
    State s;
    fresh();
    s.set(SIG_FUEL_LOW, 0, 1);
    pass(r, s, 1000);
    TEST_ASSERT_EQUAL_UINT8(0, g_nFired);
    s.set(SIG_FUEL_LOW, 1, 2);
    pass(r, s, 2000);
    TEST_ASSERT_EQUAL_UINT8(1, g_nFired);
    TEST_ASSERT_EQUAL_UINT16(0, g_fired[0]);
    TEST_ASSERT_TRUE(r.holds(0));
    // Still on: no second fire. Off and on again inside the pause: none either.
    pass(r, s, 3000);
    s.set(SIG_FUEL_LOW, 0, 4);
    pass(r, s, 4000);
    TEST_ASSERT_FALSE(r.holds(0));
    s.set(SIG_FUEL_LOW, 1, 5);
    pass(r, s, 5000);
    TEST_ASSERT_EQUAL_UINT8(1, g_nFired);
    // After the pause: the next edge fires.
    s.set(SIG_FUEL_LOW, 0, 6);
    pass(r, s, 6000);
    s.set(SIG_FUEL_LOW, 1, 7);
    pass(r, s, 2000 + 300000);
    TEST_ASSERT_EQUAL_UINT8(2, g_nFired);
    Rules::Live l;
    TEST_ASSERT_TRUE(r.live(0, l));
    TEST_ASSERT_EQUAL_UINT32(2, l.fires);
    TEST_ASSERT_TRUE(l.on && !l.gesture && l.running);
}

// True when first seen counts: the lamp lit before the board came up.
void test_true_at_first_sight_fires()
{
    RuleSet set;
    set.rows.push(signalRule("lamp", "fuel_low"));
    Rules r;
    RuleProblem p;
    TEST_ASSERT_TRUE(r.apply(set, p));
    State s;
    fresh();
    s.set(SIG_FUEL_LOW, 1, 1);
    pass(r, s, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, g_nFired);
}

// A threshold in the signal's own units; only inputs that changed are
// evaluated; the rule re-arms below the threshold.
void test_a_threshold_rule_fires_on_the_crossing()
{
    RuleSet set;
    set.rows.push(signalRule("fast", "speed > 100", 5));
    Rules r;
    RuleProblem p;
    TEST_ASSERT_TRUE(r.apply(set, p));
    TEST_ASSERT_TRUE(r.inputsOf(0).has(SIG_SPEED));
    State s;
    fresh();
    s.set(SIG_SPEED, 9000, 1);           // 90 km/h (x100)
    pass(r, s, 1000);
    TEST_ASSERT_EQUAL_UINT8(0, g_nFired);
    s.set(SIG_SPEED, 10100, 2);          // 101
    pass(r, s, 2000);
    TEST_ASSERT_EQUAL_UINT8(1, g_nFired);
    s.set(SIG_SPEED, 12000, 3);          // still above: nothing
    pass(r, s, 3000);
    TEST_ASSERT_EQUAL_UINT8(1, g_nFired);
    s.set(SIG_SPEED, 5000, 4);
    pass(r, s, 4000);
    s.set(SIG_SPEED, 10100, 5);
    pass(r, s, 9000);                    // 7 s after the first fire: the 5 s pause has run out
    TEST_ASSERT_EQUAL_UINT8(2, g_nFired);
    // An unrelated change evaluates nothing (no fire, holds unchanged).
    s.set(SIG_RPM, 2000, 6);
    pass(r, s, 10000);
    TEST_ASSERT_TRUE(r.holds(0));
}

// A gesture rule: MacroEngine detects it on the keys task; here it is only
// armed by its "only if", and it becomes a Macro for the detector.
void test_a_gesture_rule_is_armed_by_its_formula_and_becomes_a_macro()
{
    RuleSet set;
    set.rows.push(signalRule("fast", "speed > 100"));
    RuleRow g = gestureRule("source", Keys::Trigger::Combo, Keys::Action::VolUp, Keys::Action::VolDown);
    strncpy(g.when, "speed < 1", Expr::Program::MAX_SRC);
    set.rows.push(g);
    set.rows.push(gestureRule("mute3", Keys::Trigger::Sequence, Keys::Action::Mute));
    RuleProblem p;
    TEST_ASSERT_TRUE(Rules::check(set, p));
    Rules r;
    TEST_ASSERT_TRUE(r.apply(set, p));
    // No speed yet: NONE does not hold. The formula-less gesture is always armed.
    TEST_ASSERT_FALSE(r.armed(1));
    TEST_ASSERT_TRUE(r.armed(2));
    State s;
    fresh();
    s.set(SIG_SPEED, 0, 1);
    pass(r, s, 1000);
    TEST_ASSERT_TRUE(r.armed(1));
    TEST_ASSERT_EQUAL_UINT8(0, g_nFired);          // a gesture never fires from step()
    s.set(SIG_SPEED, 3000, 2);
    pass(r, s, 2000);
    TEST_ASSERT_FALSE(r.armed(1));
    // The detector's table: two macros, standing for rows 1 and 2.
    Keys::MacroTable t;
    TEST_ASSERT_EQUAL_UINT8(2, Rules::gestures(set, t, Keys::MacroTable::COUNT));
    TEST_ASSERT_EQUAL_UINT16(1, t.m[0].rule);
    TEST_ASSERT_EQUAL_UINT16(2, t.m[1].rule);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, t.m[2].rule);
    // The keys task saw the combo: armed and the pause decide, and it counts.
    TEST_ASSERT_FALSE(r.gestureFire(1, 3000));             // speed 30: not armed
    s.set(SIG_SPEED, 0, 3);
    pass(r, s, 3000);
    TEST_ASSERT_TRUE(r.gestureFire(1, 3100));
    TEST_ASSERT_TRUE(r.gestureFire(1, 3200));              // pause 0: again at once
    Rules::Live lg;
    TEST_ASSERT_TRUE(r.live(1, lg) && lg.fires == 2 && lg.gesture);
    TEST_ASSERT_TRUE(t.m[0].enabled && t.m[0].valid());
    TEST_ASSERT_EQUAL(Keys::Trigger::Combo, t.m[0].trigger);
    TEST_ASSERT_EQUAL(Keys::Action::VolDown, t.m[0].keys[1]);
    TEST_ASSERT_EQUAL(Keys::Effect::KeyCode, t.m[0].effect);
    TEST_ASSERT_EQUAL_UINT8(0x07, t.m[0].code);
    TEST_ASSERT_EQUAL_STRING("source", t.m[0].name);
    TEST_ASSERT_EQUAL(Keys::Trigger::Sequence, t.m[1].trigger);
    TEST_ASSERT_FALSE(t.m[2].enabled);
}

// check() names the row and what is wrong; a row that does not check is off,
// the others run.
void test_check_says_which_row_and_why()
{
    RuleSet set;
    RuleProblem p;
    set.rows.push(signalRule("", "speed > 1"));
    TEST_ASSERT_FALSE(Rules::check(set, p));
    TEST_ASSERT_EQUAL_STRING("rule_name", p.e.code);
    set.rows.clear();
    set.rows.push(signalRule("ok", "speed > 1"));
    set.rows.push(signalRule("short", "speed > 1", 2));
    TEST_ASSERT_FALSE(Rules::check(set, p));
    TEST_ASSERT_EQUAL_UINT16(1, p.row);
    TEST_ASSERT_EQUAL_STRING("rule_pause", p.e.code);
    set.rows[1].pauseS = 5;
    set.rows[1].act[0].effect = Keys::Effect::Text;     // text without rows
    TEST_ASSERT_FALSE(Rules::check(set, p));
    TEST_ASSERT_EQUAL_STRING("rule_action", p.e.code);
    set.rows[1].act[0].effect = Keys::Effect::Popup;
    strncpy(set.rows[1].when, "speeed > 1", Expr::Program::MAX_SRC);
    TEST_ASSERT_FALSE(Rules::check(set, p));
    TEST_ASSERT_EQUAL_STRING("expr_name", p.e.code);
    TEST_ASSERT_EQUAL_UINT16(0, p.e.pos);
    // A gesture with one member cannot be a combo.
    set.rows.push(gestureRule("one", Keys::Trigger::Combo, Keys::Action::VolUp));
    strncpy(set.rows[1].when, "speed > 1", Expr::Program::MAX_SRC);
    TEST_ASSERT_FALSE(Rules::check(set, p));
    TEST_ASSERT_EQUAL_UINT16(2, p.row);
    TEST_ASSERT_EQUAL_STRING("rule_gesture", p.e.code);
    // Applied anyway: the bad row is off, the good one runs.
    Rules r;
    TEST_ASSERT_FALSE(r.apply(set, p));
    Rules::Live l;
    TEST_ASSERT_TRUE(r.live(0, l) && l.on);
    TEST_ASSERT_TRUE(r.live(2, l) && !l.on);
    State s;
    fresh();
    s.set(SIG_SPEED, 500, 1);
    pass(r, s, 1000);
    TEST_ASSERT_EQUAL_UINT8(2, g_nFired);            // rows 0 and 1
}

// The popup's words: placeholders, or the first input as 1.6's alerts said it.
void test_the_popup_text()
{
    RuleSet set;
    set.rows.push(signalRule("low", "batt_v < 11.8"));
    set.rows.push(signalRule("lamp", "rises(fuel_low)"));
    Rules r;
    RuleProblem p;
    TEST_ASSERT_TRUE(r.apply(set, p));
    State s;
    s.set(SIG_BATT_V, 1175, 1);        // 11.75 V (x100)
    s.set(SIG_FUEL_LOW, 1, 1);
    char b[48];
    Rules::popupText(b, sizeof(b), set.rows[0], "", r.inputsOf(0), s);
    TEST_ASSERT_EQUAL_STRING("batt_v: 11.75 V ?", b);
    Rules::popupText(b, sizeof(b), set.rows[1], "", r.inputsOf(1), s);
    TEST_ASSERT_EQUAL_STRING("fuel_low: 1 ?", b);
    Rules::popupText(b, sizeof(b), set.rows[0], "AKB {batt_v} V, {speed}", r.inputsOf(0), s);
    TEST_ASSERT_EQUAL_STRING("AKB 11.75 V, --", b);
}

// The car file: every field of both kinds round-trips by name.
void test_rules_round_trip_through_the_car_file()
{
    RuleSet set;
    set.rows.push(signalRule("fast", "speed > 100 && !light_pos", 30));
    strncpy(set.rows[0].act[0].arg, "FAST {speed}", Keys::Macro::ARG_LEN - 1);
    set.rows[0].act[1].effect = Keys::Effect::Raw;
    set.rows[0].act[1].code = 0x28;
    set.rows[0].act[1].data[0] = 0x01; set.rows[0].act[1].data[1] = 0xA0;
    set.rows[0].act[1].dataLen = 2;
    set.rows[0].act[2].effect = Keys::Effect::Pane;
    strncpy(set.rows[0].act[2].arg, "tryzub", Keys::Macro::ARG_LEN - 1);
    set.rows[0].nAct = 3;
    set.rows[0].on = false;
    RuleRow g = gestureRule("mute3", Keys::Trigger::Sequence, Keys::Action::Mute);
    g.gesture.taps = 4;
    g.gesture.windowMs = 450;
    g.gesture.swallow = false;
    g.act[0].effect = Keys::Effect::Action;
    g.act[0].action = Keys::Action::SrcNext;
    g.act[0].code = 0;
    set.rows.push(g);
    JsonDocument d;
    JsonObject car = d.to<JsonObject>();
    CarFile::putRules(car, set);
    std::string text;
    serializeJson(d, text);
    TEST_ASSERT_NOT_NULL(strstr(text.c_str(), "\"when\":\"speed > 100 && !light_pos\""));
    TEST_ASSERT_NOT_NULL(strstr(text.c_str(), "\"data\":\"01 A0\""));
    TEST_ASSERT_NOT_NULL(strstr(text.c_str(), "\"trigger\":\"sequence\""));
    JsonDocument back;
    TEST_ASSERT_TRUE(deserializeJson(back, text) == DeserializationError::Ok);
    RuleSet got;
    TEST_ASSERT_NULL(CarFile::getRules(back.as<JsonObjectConst>(), got));
    TEST_ASSERT_EQUAL_UINT16(2, got.rows.size());
    const RuleRow& a = got.rows[0];
    TEST_ASSERT_EQUAL_STRING("fast", a.name);
    TEST_ASSERT_FALSE(a.on);
    TEST_ASSERT_FALSE(a.isGesture());
    TEST_ASSERT_EQUAL_UINT16(30, a.pauseS);
    TEST_ASSERT_EQUAL_UINT8(3, a.nAct);
    TEST_ASSERT_EQUAL(Keys::Effect::Popup, a.act[0].effect);
    TEST_ASSERT_EQUAL_STRING("FAST {speed}", a.act[0].arg);
    TEST_ASSERT_EQUAL(Keys::Effect::Raw, a.act[1].effect);
    TEST_ASSERT_EQUAL_UINT8(0x28, a.act[1].code);
    TEST_ASSERT_EQUAL_UINT8(2, a.act[1].dataLen);
    TEST_ASSERT_EQUAL_UINT8(0xA0, a.act[1].data[1]);
    TEST_ASSERT_EQUAL(Keys::Effect::Pane, a.act[2].effect);
    TEST_ASSERT_EQUAL_STRING("tryzub", a.act[2].arg);
    const RuleRow& b = got.rows[1];
    TEST_ASSERT_TRUE(b.isGesture());
    TEST_ASSERT_EQUAL(Keys::Trigger::Sequence, b.gesture.trigger);
    TEST_ASSERT_EQUAL(Keys::Action::Mute, b.gesture.keys[0]);
    TEST_ASSERT_EQUAL_UINT8(4, b.gesture.taps);
    TEST_ASSERT_EQUAL_UINT16(450, b.gesture.windowMs);
    TEST_ASSERT_FALSE(b.gesture.swallow);
    TEST_ASSERT_EQUAL_UINT16(0, b.pauseS);
    TEST_ASSERT_EQUAL(Keys::Effect::Action, b.act[0].effect);
    TEST_ASSERT_EQUAL(Keys::Action::SrcNext, b.act[0].action);
    RuleProblem p;
    TEST_ASSERT_TRUE(Rules::check(got, p));
    // A bad row refuses the whole list, with its key.
    JsonDocument bad;
    deserializeJson(bad, "{\"rules\":[{\"name\":\"x\",\"when\":\"speed\",\"do\":[{\"effect\":\"raw\",\"codeHex\":\"zz\"}]}]}");
    TEST_ASSERT_EQUAL_STRING("do.codeHex: param", CarFile::getRules(bad.as<JsonObjectConst>(), got));
    TEST_ASSERT_EQUAL_UINT16(0, got.rows.size());
    JsonDocument none;
    deserializeJson(none, "{\"v\":1}");
    TEST_ASSERT_EQUAL_STRING("rules: missing", CarFile::getRules(none.as<JsonObjectConst>(), got));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_factory_rules_are_the_old_alerts);
    RUN_TEST(test_true_at_first_sight_fires);
    RUN_TEST(test_a_threshold_rule_fires_on_the_crossing);
    RUN_TEST(test_a_gesture_rule_is_armed_by_its_formula_and_becomes_a_macro);
    RUN_TEST(test_check_says_which_row_and_why);
    RUN_TEST(test_the_popup_text);
    RUN_TEST(test_rules_round_trip_through_the_car_file);
    return UNITY_END();
}
