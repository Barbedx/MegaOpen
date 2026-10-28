#include <unity.h>
#include <string.h>
#include "vehicle/Formulas.h"
#include "vehicle/Signals.h"

void setUp() {}
void tearDown() {}

// Signals computed by formulas (src/vehicle/Formulas.h): the factory's, which
// took over fuel_pct and range_calc from Derived, and the owner's own.
using namespace Vehicle;

namespace
{
    FormulaRow row(const char* name, const char* src, const char* unit = "", uint8_t dec = 0, bool boolean = false)
    {
        FormulaRow r;
        strncpy(r.name, name, USER_NAME);
        strncpy(r.src, src, Expr::Program::MAX_SRC);
        strncpy(r.unit, unit, USER_UNIT);
        r.decimals = dec;
        r.boolean = boolean;
        return r;
    }
    // One pass: what changed, then the formulas.
    void pass(Formulas& f, State& s, uint32_t now, bool tick = false)
    {
        Expr::SigSet ch;
        s.takeChanged(ch);
        f.step(s, ch, now, tick);
    }
}

// The factory rows give what Derived gave: 31 L of 60 is 51.7 %, and 40 L at
// 14.4 L/100km goes 277 km.
void test_the_factory_formulas_replace_derived()
{
    FormulaSet set;
    set.factory();
    Formulas f;
    FormulaProblem p;
    TEST_ASSERT_TRUE(f.apply(set, p));
    TEST_ASSERT_TRUE(f.targets().has(SIG_FUEL_PCT) && f.targets().has(SIG_RANGE_CALC));
    State s;
    s.set(SIG_FUEL_LEVEL, 31, 1);
    pass(f, s, 10);
    TEST_ASSERT_EQUAL_INT32(517, s.get(SIG_FUEL_PCT));          // x10, to the nearest
    TEST_ASSERT_FALSE(s.has(SIG_RANGE_CALC));                   // no average yet: no value
    s.set(SIG_FUEL_LEVEL, 40, 20);
    s.set(SIG_CONS_AVG, 144, 20);                              // 14.4 L/100km
    pass(f, s, 20);
    TEST_ASSERT_EQUAL_INT32(278, s.get(SIG_RANGE_CALC));        // 277.8, to the nearest
    Expr::SigSet in;
    TEST_ASSERT_TRUE(f.inputsOf(SIG_RANGE_CALC, in));
    TEST_ASSERT_TRUE(in.has(SIG_FUEL_LEVEL) && in.has(SIG_CONS_AVG));
    TEST_ASSERT_FALSE(f.inputsOf(SIG_SPEED, in));
}

// A new name takes a user slot with its unit and decimals, and reads like any
// signal: in a later formula, in the same pass.
void test_a_user_signal_is_a_signal()
{
    FormulaSet set;
    set.rows.push(row("half_tank", "fuel_level / 2", "L", 1));
    set.rows.push(row("low_half", "half_tank < 10", "", 0, true));
    Formulas f;
    FormulaProblem p;
    TEST_ASSERT_TRUE(f.apply(set, p));
    const uint8_t h = sigByName("half_tank"), l = sigByName("low_half");
    TEST_ASSERT_TRUE(isUser(h) && isUser(l));
    TEST_ASSERT_EQUAL_STRING("L", meta(h).unit);
    TEST_ASSERT_EQUAL_UINT8(1, meta(h).decimals);
    TEST_ASSERT_TRUE(meta(l).boolean);
    State s;
    s.set(SIG_FUEL_LEVEL, 15, 1);
    pass(f, s, 10);
    TEST_ASSERT_EQUAL_INT32(75, s.get(h));                      // 7.5 L, x10
    TEST_ASSERT_EQUAL_INT32(1, s.get(l));                       // the second row saw the first
    // Its inputs change: both follow. Nothing changed: nothing is evaluated.
    s.set(SIG_FUEL_LEVEL, 30, 2);
    pass(f, s, 20);
    TEST_ASSERT_EQUAL_INT32(150, s.get(h));
    TEST_ASSERT_EQUAL_INT32(0, s.get(l));
    const uint32_t stamp = s.stamp(h);
    pass(f, s, 30);
    TEST_ASSERT_EQUAL_UINT32(stamp, s.stamp(h));
    // No input: no value.
    s.clear(SIG_FUEL_LEVEL);
    pass(f, s, 40);
    TEST_ASSERT_FALSE(s.has(h));
    TEST_ASSERT_FALSE(s.has(l));
}

// A name keeps its slot when the set changes around it: what reads its id
// (the HU fields, a binding) does not move under it.
void test_a_name_keeps_its_slot()
{
    Formulas f;
    FormulaProblem p;
    FormulaSet a;
    a.rows.push(row("one", "1"));
    a.rows.push(row("two", "2"));
    TEST_ASSERT_TRUE(f.apply(a, p));
    const uint8_t two = sigByName("two");
    FormulaSet b;
    b.rows.push(row("two", "2"));
    b.rows.push(row("three", "3"));
    TEST_ASSERT_TRUE(f.apply(b, p));
    TEST_ASSERT_EQUAL_UINT8(two, sigByName("two"));
    TEST_ASSERT_EQUAL_UINT8(SIG_COUNT, sigByName("one"));        // gone
    TEST_ASSERT_TRUE(isUser(sigByName("three")));
    // The longest name (23) is a name like any other.
    const char* longest = "abcdefghijklmnopqrstuvw";
    FormulaSet c;
    c.rows.push(row(longest, "1"));
    c.rows.push(row("two", "2"));
    TEST_ASSERT_TRUE(f.apply(c, p));
    TEST_ASSERT_TRUE(isUser(sigByName(longest)));
    TEST_ASSERT_EQUAL_UINT8(two, sigByName("two"));
    TEST_ASSERT_TRUE(f.apply(c, p));
    TEST_ASSERT_EQUAL_STRING(longest, meta(sigByName(longest)).name);
}

// A target bound to CAN or a constant is fed by that: its row stands down.
void test_a_bound_target_stands_down()
{
    FormulaSet set;
    set.factory();
    Formulas f;
    FormulaProblem p;
    TEST_ASSERT_TRUE(f.apply(set, p));
    Expr::SigSet b;
    b.set(SIG_FUEL_PCT);
    f.setBound(b);
    State s;
    s.set(SIG_FUEL_PCT, 999, 1);                               // from its CAN row
    s.set(SIG_FUEL_LEVEL, 30, 1);
    pass(f, s, 10);
    TEST_ASSERT_EQUAL_INT32(999, s.get(SIG_FUEL_PCT));
}

// Functions of time run on the tick, with nothing changed.
void test_a_timed_formula_runs_on_the_tick()
{
    FormulaSet set;
    set.rows.push(row("long_stop", "held(speed < 1, 2)", "", 0, true));
    Formulas f;
    FormulaProblem p;
    TEST_ASSERT_TRUE(f.apply(set, p));
    const uint8_t ls = sigByName("long_stop");
    State s;
    s.set(SIG_SPEED, 0, 1);
    pass(f, s, 1000, true);
    TEST_ASSERT_EQUAL_INT32(0, s.get(ls));
    pass(f, s, 2000);                                           // no tick, nothing changed
    TEST_ASSERT_EQUAL_INT32(0, s.get(ls));
    pass(f, s, 3100, true);
    TEST_ASSERT_EQUAL_INT32(1, s.get(ls));
}

// What check() refuses, with which row; a bad row is off, the rest run.
void test_check_says_which_row()
{
    FormulaProblem p;
    FormulaSet set;
    set.rows.push(row("ok_one", "speed * 2"));
    set.rows.push(row("Bad-Name", "1"));
    TEST_ASSERT_FALSE(Formulas::check(set, p));
    TEST_ASSERT_EQUAL_UINT16(1, p.row);
    TEST_ASSERT_EQUAL_STRING("formula_name", p.e.code);
    set.rows[1] = row("avg", "1");                              // a function's name
    TEST_ASSERT_FALSE(Formulas::check(set, p));
    TEST_ASSERT_EQUAL_STRING("formula_name", p.e.code);
    set.rows[1] = row("ok_one", "1");
    TEST_ASSERT_FALSE(Formulas::check(set, p));
    TEST_ASSERT_EQUAL_STRING("formula_twice", p.e.code);
    set.rows[1] = row("later", "ok_one + nothing");
    TEST_ASSERT_FALSE(Formulas::check(set, p));
    TEST_ASSERT_EQUAL_UINT16(1, p.row);
    TEST_ASSERT_EQUAL_STRING("expr_name", p.e.code);
    TEST_ASSERT_EQUAL_UINT16(9, p.e.pos);
    // A name defined further down is known (it runs a pass later).
    set.rows[1] = row("first", "second + 1");
    set.rows.push(row("second", "speed"));
    TEST_ASSERT_TRUE(Formulas::check(set, p));
    // Applied with a bad row: false, and the good rows run.
    set.rows[1] = row("first", "speed +");
    Formulas f;
    TEST_ASSERT_FALSE(f.apply(set, p));
    TEST_ASSERT_EQUAL_UINT16(1, p.row);
    State s;
    s.set(SIG_SPEED, 1000, 1);                                  // 10 km/h
    pass(f, s, 10);
    TEST_ASSERT_EQUAL_INT32(20, s.get(sigByName("ok_one")));
    TEST_ASSERT_FALSE(s.has(sigByName("first")));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_factory_formulas_replace_derived);
    RUN_TEST(test_a_user_signal_is_a_signal);
    RUN_TEST(test_a_name_keeps_its_slot);
    RUN_TEST(test_a_bound_target_stands_down);
    RUN_TEST(test_a_timed_formula_runs_on_the_tick);
    RUN_TEST(test_check_says_which_row);
    return UNITY_END();
}
