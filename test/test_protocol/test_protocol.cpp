#include <unity.h>
#include <string.h>
#include <initializer_list>
#include "proto/canbox/Protocol.h"
#include "proto/canbox/FieldView.h"
#include "vehicle/VehicleState.h"

void setUp() {}
void tearDown() {}

// The map layer knows a head-unit protocol only through FieldProtocol
// (docs/design/hu-mapping.md, step 5): a second protocol is a module with
// its own field list and defaults, and the field map, the rule lookup and
// FieldView work on it unchanged. The stub has no wire format.
using namespace Vehicle;
using namespace Canbox;

namespace
{
    struct Stub final : FieldProtocol {
        const char* id() const override { return "stub"; }
        uint8_t fieldCount() const override { return 2; }
        TargetView field(uint8_t i) const override
        {
            TargetView v{};
            v.id  = i ? "eng_temp" : "speed";
            v.sig = i ? SIG_ENG_TEMP : SIG_SPEED;
            v.fw  = true;
            v.msg = NO_MSG;
            return v;
        }
        const FieldMap& defaults() const override
        {
            static const FieldMap m = [] {
                FieldMap d;
                FieldRule r;
                r.field = SIG_ENG_TEMP;
                r.kind = FK_OFF;
                d.set(r);
                return d;
            }();
            return m;
        }
    };
}

void test_a_second_protocol_runs_the_map_unchanged()
{
    const Stub p;
    TEST_ASSERT_EQUAL_INT(1, p.findField("eng_temp"));
    TEST_ASSERT_EQUAL_INT(-1, p.findField("rpm"));
    State s;
    s.set(SIG_SPEED, 4700, 1);
    s.set(SIG_ENG_TEMP, 90, 1);
    FieldMap own, eff;
    // Its default: the coolant field starts off; the speed is its own signal.
    p.withDefaults(own, eff);
    const FieldView v(s, &eff);
    TEST_ASSERT_TRUE(v.has(SIG_SPEED));
    TEST_ASSERT_FALSE(v.has(SIG_ENG_TEMP));
    TEST_ASSERT_EQUAL_UINT8(FK_OFF, p.ruleFor(own, SIG_ENG_TEMP)->kind);
    TEST_ASSERT_NULL(p.ruleFor(own, SIG_SPEED));
    // The owner's rule goes over the default: a constant.
    FieldRule k;
    k.field = SIG_ENG_TEMP;
    k.kind = FK_CONST;
    k.add = 70;
    own.set(k);
    p.withDefaults(own, eff);
    const FieldView w(s, &eff);
    TEST_ASSERT_TRUE(w.has(SIG_ENG_TEMP));
    TEST_ASSERT_EQUAL_INT32(70, w.get(SIG_ENG_TEMP));
    TEST_ASSERT_EQUAL_UINT8(FK_CONST, p.ruleFor(own, SIG_ENG_TEMP)->kind);
}

void test_the_active_protocol_is_raise_112()
{
    const FieldProtocol& p = activeProtocol();
    TEST_ASSERT_EQUAL_STRING("raise112", p.id());
    TEST_ASSERT_TRUE(p.fieldCount() > 50);
    const int i = p.findField("speed");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL_UINT8(SIG_SPEED, p.field((uint8_t)i).sig);
    // The fields that start off (Rav4Emitter::defaults).
    FieldMap none;
    for (const uint8_t f : {SIG_ENG_TEMP, SIG_PRND, SIG_FUEL_PCT})
        TEST_ASSERT_EQUAL_UINT8(FK_OFF, p.ruleFor(none, f)->kind);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_second_protocol_runs_the_map_unchanged);
    RUN_TEST(test_the_active_protocol_is_raise_112);
    return UNITY_END();
}
