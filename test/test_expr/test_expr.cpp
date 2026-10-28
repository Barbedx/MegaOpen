#include <unity.h>
#include <string.h>
#include "vehicle/Expr.h"
#include "vehicle/ExprState.h"

void setUp() {}
void tearDown() {}

// The formula language (src/vehicle/Expr.h): what the owner types on the page
// for a user signal, a rule's condition, a head-unit binding.
using namespace Expr;

namespace
{
    // Four signals in their own units x SCALE: speed (km/h), rpm, lights
    // (yes/no) and fuel (L). `have` says which have a value.
    const char* const NAMES[] = {"speed", "rpm", "lights", "fuel"};
    Num      g_v[4];
    bool     g_have[4];
    uint32_t g_age[4];

    int resolve(const char* n, size_t len, void*)
    {
        for (int i = 0; i < 4; ++i)
            if (strlen(NAMES[i]) == len && !strncmp(NAMES[i], n, len)) return i;
        return -1;
    }
    bool read(uint8_t id, Num& v, uint32_t& age, void*)
    {
        if (id >= 4 || !g_have[id]) return false;
        v = g_v[id];
        age = g_age[id];
        return true;
    }
    void car(double speed, double rpm, bool lights, double fuel)
    {
        g_v[0] = (Num)(speed * SCALE); g_v[1] = (Num)(rpm * SCALE);
        g_v[2] = lights ? SCALE : 0;   g_v[3] = (Num)(fuel * SCALE);
        for (int i = 0; i < 4; ++i) { g_have[i] = true; g_age[i] = 0; }
    }

    struct F {
        Program p;
        Program::State st;
        Error e;
        bool ok;
        explicit F(const char* src) { ok = p.compile(src, resolve, nullptr, e) && p.reset(st); }
        Num at(uint32_t now = 0) { return p.eval(st, read, nullptr, now); }
    };
    Num v(const char* src, uint32_t now = 0)
    {
        F f(src);
        TEST_ASSERT_TRUE_MESSAGE(f.ok, f.e.code ? f.e.code : src);
        return f.at(now);
    }
    void refused(const char* src, const char* code, uint16_t pos)
    {
        F f(src);
        TEST_ASSERT_FALSE_MESSAGE(f.ok, src);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(code, f.e.code, src);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(pos, f.e.pos, src);
        TEST_ASSERT_TRUE(f.p.empty());
    }
}

void test_arithmetic_in_the_signals_own_units()
{
    car(47, 2500, true, 31.5);
    TEST_ASSERT_EQUAL_INT64(52500, v("fuel * 100 / 60"));        // 52.5 %
    TEST_ASSERT_EQUAL_INT64(7 * SCALE, v("1 + 2 * 3"));          // precedence
    TEST_ASSERT_EQUAL_INT64(9 * SCALE, v("(1 + 2) * 3"));
    TEST_ASSERT_EQUAL_INT64(-3 * SCALE, v("-(1 + 2)"));
    TEST_ASSERT_EQUAL_INT64(1500, v("3 / 2"));                   // fixed point, not integer division
    TEST_ASSERT_EQUAL_INT64(1 * SCALE, v("7 % 3"));
    TEST_ASSERT_EQUAL_INT64(250, v("0.25"));
    TEST_ASSERT_EQUAL_INT64(1235, v("1.2345"));                  // the 4th decimal rounds
    TEST_ASSERT_EQUAL_INT64(-2 * SCALE, v("-2"));
}

void test_conditions_and_logic()
{
    car(47, 2500, false, 31.5);
    TEST_ASSERT_TRUE(holds(v("speed > 10 && !lights")));
    TEST_ASSERT_TRUE(holds(v("speed > 10 and not lights")));
    TEST_ASSERT_FALSE(holds(v("speed < 10 || lights")));
    TEST_ASSERT_TRUE(holds(v("speed >= 47 and speed <= 47 and speed == 47 and rpm != 0")));
    TEST_ASSERT_TRUE(holds(v("true")));
    TEST_ASSERT_FALSE(holds(v("false")));
    TEST_ASSERT_EQUAL_INT64(SCALE, v("2 > 1"));                  // a comparison is 1 or 0
}

// No value: carried through arithmetic and comparisons, decided by && and ||
// when the other side decides it, and never "holds".
void test_no_value_carries_through()
{
    car(47, 2500, false, 31.5);
    g_have[3] = false;                                            // no fuel reading yet
    TEST_ASSERT_EQUAL_INT64(NONE, v("fuel * 100 / 60"));
    TEST_ASSERT_EQUAL_INT64(NONE, v("fuel > 10"));
    TEST_ASSERT_FALSE(holds(v("fuel > 10")));
    TEST_ASSERT_FALSE(holds(v("!(fuel > 10)")));                 // not "not low": unknown
    TEST_ASSERT_EQUAL_INT64(0, v("fuel > 10 && speed < 1"));     // the known side is false
    TEST_ASSERT_EQUAL_INT64(SCALE, v("fuel > 10 || speed > 1"));  // the known side is true
    TEST_ASSERT_EQUAL_INT64(NONE, v("fuel > 10 && speed > 1"));
    TEST_ASSERT_EQUAL_INT64(0, v("has(fuel)"));
    TEST_ASSERT_EQUAL_INT64(SCALE, v("has(speed)"));
    TEST_ASSERT_EQUAL_INT64(NONE, v("1 / 0"));
    TEST_ASSERT_EQUAL_INT64(5 * SCALE, v("if(has(fuel), fuel, 5)"));
}

void test_pure_functions()
{
    car(47, 2500, false, 31.5);
    TEST_ASSERT_EQUAL_INT64(2 * SCALE, v("min(5, 2, 9)"));
    TEST_ASSERT_EQUAL_INT64(9 * SCALE, v("max(5, 2, 9)"));
    TEST_ASSERT_EQUAL_INT64(3 * SCALE, v("abs(-3)"));
    TEST_ASSERT_EQUAL_INT64(100 * SCALE, v("clamp(speed * 3, 0, 100)"));
    TEST_ASSERT_EQUAL_INT64(SCALE, v("if(speed > 10, 1, 2)"));
    TEST_ASSERT_EQUAL_INT64(32 * SCALE, v("round(fuel)"));
    TEST_ASSERT_EQUAL_INT64(-32 * SCALE, v("round(-31.5)"));
    // Straight lines between the points, flat past the ends.
    TEST_ASSERT_EQUAL_INT64(15 * SCALE, v("lookup(15, 10, 10, 20, 20)"));
    TEST_ASSERT_EQUAL_INT64(25 * SCALE, v("lookup(5, 10, 25, 20, 30, 40, 0)"));
    TEST_ASSERT_EQUAL_INT64(15 * SCALE, v("lookup(30, 10, 25, 20, 30, 40, 0)"));
    TEST_ASSERT_EQUAL_INT64(0, v("lookup(99, 10, 25, 20, 30, 40, 0)"));
    g_age[0] = 4500;
    TEST_ASSERT_EQUAL_INT64(4500, v("age(speed)"));              // 4.5 s
}

// Functions with state: each call keeps its own, evaluation to evaluation.
void test_functions_of_change()
{
    car(10, 0, false, 30);
    F d("delta(fuel)"), c("changed(speed)"), r("rises(lights)"), fl("falls(lights)");
    TEST_ASSERT_TRUE(d.ok && c.ok && r.ok && fl.ok);
    TEST_ASSERT_EQUAL_INT64(0, d.at());
    TEST_ASSERT_EQUAL_INT64(0, c.at());
    TEST_ASSERT_EQUAL_INT64(0, r.at());
    TEST_ASSERT_EQUAL_INT64(0, fl.at());
    car(12, 0, true, 29.5);
    TEST_ASSERT_EQUAL_INT64(-500, d.at());
    TEST_ASSERT_EQUAL_INT64(SCALE, c.at());
    TEST_ASSERT_EQUAL_INT64(SCALE, r.at());
    TEST_ASSERT_EQUAL_INT64(0, fl.at());
    TEST_ASSERT_EQUAL_INT64(0, r.at());                            // once, on the edge
    TEST_ASSERT_EQUAL_INT64(0, c.at());
    car(12, 0, false, 29.5);
    TEST_ASSERT_EQUAL_INT64(SCALE, fl.at());
    // Two calls in one formula keep apart.
    F two("delta(speed) + delta(fuel)");
    TEST_ASSERT_EQUAL_UINT16(2, two.p.stateSlots());
}

void test_functions_of_time()
{
    car(10, 0, false, 30);
    F h("held(speed > 5, 3)"), rt("rate(fuel)"), in("integral(speed)"), av("avg(speed, 10)");
    TEST_ASSERT_TRUE(h.ok && rt.ok && in.ok && av.ok);
    TEST_ASSERT_TRUE(h.p.timed() && rt.p.timed() && in.p.timed() && av.p.timed());
    TEST_ASSERT_EQUAL_INT64(0, h.at(1000));
    TEST_ASSERT_EQUAL_INT64(0, h.at(3999));
    TEST_ASSERT_EQUAL_INT64(SCALE, h.at(4000));                    // held 3 s
    car(1, 0, false, 30);
    TEST_ASSERT_EQUAL_INT64(0, h.at(4500));                        // let go: starts again
    car(10, 0, false, 30);
    TEST_ASSERT_EQUAL_INT64(0, h.at(5000));
    TEST_ASSERT_EQUAL_INT64(SCALE, h.at(8000));
    // 30 L to 29 L in 10 s: -0.1 L/s.
    car(10, 0, false, 30);
    TEST_ASSERT_EQUAL_INT64(NONE, rt.at(0));
    car(10, 0, false, 29);
    TEST_ASSERT_EQUAL_INT64(-100, rt.at(10000));
    // 10 km/h for 3600 s: 36000 km/h x s, i.e. 10 km.
    car(10, 0, false, 30);
    TEST_ASSERT_EQUAL_INT64(0, in.at(0));
    TEST_ASSERT_EQUAL_INT64(36000 * SCALE, in.at(3600000));
    // The average moves toward the new value with its time constant.
    car(0, 0, false, 30);
    TEST_ASSERT_EQUAL_INT64(0, av.at(0));
    car(100, 0, false, 30);
    TEST_ASSERT_INT64_WITHIN(1, 63212, av.at(10000));             // one time constant: 1 - 1/e
    // The same in many small steps as in one big one.
    F av2("avg(speed, 10)");
    car(0, 0, false, 30);
    av2.at(0);
    car(100, 0, false, 30);
    for (uint32_t t = 100; t <= 10000; t += 100) av2.at(t);
    TEST_ASSERT_INT64_WITHIN(5, 63212, av2.at(10000));
    const Num later = av.at(100000);
    TEST_ASSERT_TRUE(later > 99 * SCALE && later <= 100 * SCALE);
}

// Refused on save, with where: what the editor underlines.
void test_errors_say_what_and_where()
{
    refused("", "expr_empty", 0);
    refused("speed >", "expr_end", 7);
    refused("speed > > 3", "expr_syntax", 8);
    refused("sped > 3", "expr_name", 0);
    refused("speed + warp(1)", "expr_func", 8);
    refused("min(1)", "expr_args", 0);
    refused("abs(1, 2)", "expr_args", 0);
    refused("lookup(1, 2, 3, 4)", "expr_args", 0);
    refused("held(speed > 5, speed)", "expr_const", 0);
    refused("avg(speed, 0)", "expr_const", 0);
    refused("age(speed + 1)", "expr_name", 0);
    refused("(speed + 1", "expr_paren", 10);
    refused("speed 3", "expr_syntax", 6);
    refused("3speed", "expr_syntax", 1);
    char deep[200] = "";
    for (int i = 0; i < 20; ++i) strcat(deep, "(1+");
    strcat(deep, "1");
    for (int i = 0; i < 20; ++i) strcat(deep, ")");
    F f(deep);
    TEST_ASSERT_FALSE(f.ok);
    TEST_ASSERT_EQUAL_STRING("expr_deep", f.e.code);
    char lng[300];
    memset(lng, '1', sizeof(lng) - 1);
    lng[sizeof(lng) - 1] = 0;
    refused(lng, "expr_long", Program::MAX_SRC);
}

// What a formula reads: the rule engine re-evaluates only when one changed.
void test_inputs_are_the_signals_it_reads()
{
    F f("speed > 10 && !lights");
    TEST_ASSERT_TRUE(f.ok);
    TEST_ASSERT_TRUE(f.p.inputs().has(0));
    TEST_ASSERT_FALSE(f.p.inputs().has(1));
    TEST_ASSERT_TRUE(f.p.inputs().has(2));
    TEST_ASSERT_FALSE(f.p.timed());
    SigSet rpm;
    rpm.set(1);
    TEST_ASSERT_FALSE(f.p.inputs().any(rpm));
}

// A signal's own integer, both ways: speed is km/h x100 in State.
void test_raw_values_both_ways()
{
    TEST_ASSERT_EQUAL_INT64(47 * SCALE, fromRaw(4700, 2));
    TEST_ASSERT_EQUAL_INT64(-40 * SCALE, fromRaw(-40, 0));
    int32_t raw = 0;
    TEST_ASSERT_TRUE(toRaw(52500, 1, raw));
    TEST_ASSERT_EQUAL_INT32(525, raw);                             // 52.5 %, x10
    TEST_ASSERT_TRUE(toRaw(1999, 0, raw));
    TEST_ASSERT_EQUAL_INT32(2, raw);                               // to the nearest
    TEST_ASSERT_TRUE(toRaw(-1500, 0, raw));
    TEST_ASSERT_EQUAL_INT32(-2, raw);
    TEST_ASSERT_FALSE(toRaw(NONE, 0, raw));
    TEST_ASSERT_FALSE(toRaw((Num)5000000000LL * SCALE, 0, raw));   // past int32
}

// Over the car's State: names are the signals', values in their own units
// (speed is x100 in State, km/h in a formula).
void test_formulas_read_the_cars_signals()
{
    Vehicle::State st;
    st.set(Vehicle::SIG_SPEED, 4700, 1000);
    Vehicle::ExprCtx c{&st, 1500};
    Program p;
    Program::State ps;
    Error e;
    TEST_ASSERT_TRUE(p.compile("speed > 46.9 && speed < 47.1", Vehicle::exprResolve, nullptr, e));
    TEST_ASSERT_TRUE(p.reset(ps));
    TEST_ASSERT_TRUE(holds(p.eval(ps, Vehicle::exprRead, &c, 1500)));
    TEST_ASSERT_TRUE(p.compile("age(speed)", Vehicle::exprResolve, nullptr, e));
    TEST_ASSERT_EQUAL_INT64(500, p.eval(ps, Vehicle::exprRead, &c, 1500));    // 0.5 s
    TEST_ASSERT_FALSE(p.compile("speeed > 1", Vehicle::exprResolve, nullptr, e));
    TEST_ASSERT_EQUAL_STRING("expr_name", e.code);
    // No value yet: NONE.
    TEST_ASSERT_TRUE(p.compile("rpm", Vehicle::exprResolve, nullptr, e));
    TEST_ASSERT_EQUAL_INT64(NONE, p.eval(ps, Vehicle::exprRead, &c, 1500));
}

// What changed since the last take: a new value, a first value, a pin and an
// unpin; the same value again is not a change.
void test_state_says_what_changed()
{
    Vehicle::State st;
    SigSet ch;
    st.takeChanged(ch);
    st.set(Vehicle::SIG_SPEED, 100, 1);
    st.set(Vehicle::SIG_RPM, 0, 1);                               // a first 0 is a change too
    st.takeChanged(ch);
    TEST_ASSERT_TRUE(ch.has(Vehicle::SIG_SPEED));
    TEST_ASSERT_TRUE(ch.has(Vehicle::SIG_RPM));
    st.set(Vehicle::SIG_SPEED, 100, 2);
    st.takeChanged(ch);
    TEST_ASSERT_FALSE(ch.has(Vehicle::SIG_SPEED));
    st.pin(Vehicle::SIG_SPEED, 5);
    st.takeChanged(ch);
    TEST_ASSERT_TRUE(ch.has(Vehicle::SIG_SPEED));
    st.set(Vehicle::SIG_SPEED, 200, 3);                           // under a pin: get() did not change
    st.takeChanged(ch);
    TEST_ASSERT_FALSE(ch.has(Vehicle::SIG_SPEED));
    st.unpin(Vehicle::SIG_SPEED);
    st.takeChanged(ch);
    TEST_ASSERT_TRUE(ch.has(Vehicle::SIG_SPEED));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_arithmetic_in_the_signals_own_units);
    RUN_TEST(test_conditions_and_logic);
    RUN_TEST(test_no_value_carries_through);
    RUN_TEST(test_pure_functions);
    RUN_TEST(test_functions_of_change);
    RUN_TEST(test_functions_of_time);
    RUN_TEST(test_errors_say_what_and_where);
    RUN_TEST(test_inputs_are_the_signals_it_reads);
    RUN_TEST(test_raw_values_both_ways);
    RUN_TEST(test_formulas_read_the_cars_signals);
    RUN_TEST(test_state_says_what_changed);
    return UNITY_END();
}
