#include <unity.h>
#include "proto/canbox/FieldMap.h"
#include "proto/canbox/FieldView.h"
#include "vehicle/Signals.h"

// The head-unit field map (docs/design/hu-mapping.md, step 2): a field with no
// rule is its own signal; a rule takes another signal scaled, a constant, or off.

using namespace Canbox;
using namespace Vehicle;

void setUp() {}
void tearDown() {}

void test_no_rule_is_the_identity()
{
    State s;
    s.set(SIG_AVG_SPEED, 4437, 1);
    FieldMap m;
    FieldView v(s, &m);
    TEST_ASSERT_TRUE(v.has(SIG_AVG_SPEED));
    TEST_ASSERT_EQUAL_INT32(4437, v.get(SIG_AVG_SPEED));
    TEST_ASSERT_FALSE(v.has(SIG_RPM));
}

void test_a_field_from_another_signal_scaled()
{
    State s;
    s.set(SIG_FUEL_LEVEL, 30, 1);                 // litres
    FieldMap m;
    FieldRule r;
    r.field = SIG_RANGE; r.kind = FK_SIGNAL; r.src = SIG_FUEL_LEVEL; r.mul = 1000; r.div = 60; r.add = 0;
    TEST_ASSERT_TRUE(m.set(r));
    FieldView v(s, &m);
    TEST_ASSERT_TRUE(v.has(SIG_RANGE));
    TEST_ASSERT_EQUAL_INT32(500, v.get(SIG_RANGE));   // 30 * 1000 / 60
    s.unpin(SIG_FUEL_LEVEL);
    State empty;
    FieldView e(empty, &m);
    TEST_ASSERT_FALSE(e.has(SIG_RANGE));             // the source has nothing: nor the field
}

void test_const_and_off()
{
    State s;
    s.set(SIG_OUT_TEMP, 12, 1);
    FieldMap m;
    FieldRule k; k.field = SIG_ENG_TEMP; k.kind = FK_CONST; k.add = 90;
    FieldRule off; off.field = SIG_OUT_TEMP; off.kind = FK_OFF;
    TEST_ASSERT_TRUE(m.set(k));
    TEST_ASSERT_TRUE(m.set(off));
    FieldView v(s, &m);
    TEST_ASSERT_TRUE(v.has(SIG_ENG_TEMP));
    TEST_ASSERT_EQUAL_INT32(90, v.get(SIG_ENG_TEMP));
    TEST_ASSERT_TRUE(v.pinned(SIG_ENG_TEMP));
    TEST_ASSERT_FALSE(v.has(SIG_OUT_TEMP));
    m.clear(SIG_OUT_TEMP);
    TEST_ASSERT_TRUE(FieldView(s, &m).has(SIG_OUT_TEMP));
}

void test_bad_rules_are_refused()
{
    FieldMap m;
    FieldRule r; r.field = SIG_RANGE; r.kind = FK_SIGNAL; r.src = 0xFF;
    TEST_ASSERT_FALSE(m.set(r));                       // no source
    r.src = SIG_FUEL_LEVEL; r.div = 0;
    TEST_ASSERT_FALSE(m.set(r));                       // divide by zero
    r.div = 1; r.kind = 9;
    TEST_ASSERT_FALSE(m.set(r));
    TEST_ASSERT_EQUAL_UINT8(0, m.count());
}

void test_the_blob_round_trips_and_a_bad_one_is_refused()
{
    FieldMap m;
    FieldRule a; a.field = SIG_RANGE; a.kind = FK_SIGNAL; a.src = SIG_FUEL_LEVEL; a.mul = -3; a.div = 7; a.add = -100000;
    FieldRule b; b.field = SIG_ENG_TEMP; b.kind = FK_CONST; b.add = 95;
    m.set(a); m.set(b);
    uint8_t blob[FieldMap::BLOB_MAX];
    const size_t n = m.pack(blob, sizeof(blob));
    TEST_ASSERT_EQUAL(2 + 2 * FieldMap::RULE_BYTES, n);
    FieldMap back;
    TEST_ASSERT_TRUE(back.unpack(blob, n));
    TEST_ASSERT_EQUAL_UINT8(2, back.count());
    const FieldRule* r = back.find(SIG_RANGE);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_INT16(-3, r->mul);
    TEST_ASSERT_EQUAL_INT16(7, r->div);
    TEST_ASSERT_EQUAL_INT32(-100000, r->add);
    blob[1] = 3;                                       // says 3, holds 2
    TEST_ASSERT_FALSE(back.unpack(blob, n));
    TEST_ASSERT_EQUAL_UINT8(2, back.count());           // unchanged
}

void test_the_map_is_bounded()
{
    FieldMap m;
    for (uint8_t i = 0; i < FieldMap::MAX; ++i) {
        FieldRule r; r.field = i; r.kind = FK_OFF;
        TEST_ASSERT_TRUE(m.set(r));
    }
    FieldRule r; r.field = 200; r.kind = FK_OFF;
    TEST_ASSERT_FALSE(m.set(r));
    r.field = 5;                                        // replacing one still works
    TEST_ASSERT_TRUE(m.set(r));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_rule_is_the_identity);
    RUN_TEST(test_a_field_from_another_signal_scaled);
    RUN_TEST(test_const_and_off);
    RUN_TEST(test_bad_rules_are_refused);
    RUN_TEST(test_the_blob_round_trips_and_a_bad_one_is_refused);
    RUN_TEST(test_the_map_is_bounded);
    return UNITY_END();
}
