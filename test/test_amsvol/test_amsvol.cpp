// How many AMS commands a volume press sends (sinks/AmsVolumeStep.h). On the
// phone a wrong count is either a press that does nothing audible or one
// that jumps the whole range, and each command costs a BLE round trip, so
// the rule is pinned here.
#include <unity.h>
#include "sinks/AmsVolumeStep.h"

using Sinks::AmsVolumeStep;

void setUp() {}
void tearDown() {}

// The first press is one command: the measurement, never a jump.
void test_it_starts_at_one_command()
{
    AmsVolumeStep s;
    TEST_ASSERT_EQUAL_UINT8(1, s.steps());
    TEST_ASSERT_FALSE(s.learned());
}

// A command that moves a whole button step stays one command a press: the
// fifteen writes this replaces were fourteen too many.
void test_a_button_sized_step_is_one_command()
{
    AmsVolumeStep s;
    TEST_ASSERT_TRUE(s.observe(0.5f, 0.5625f, 1, true));
    TEST_ASSERT_EQUAL_UINT8(1, s.steps());
}

// A tenth of a button step a command: ten commands a press.
void test_a_small_step_scales_up()
{
    AmsVolumeStep s;
    TEST_ASSERT_TRUE(s.observe(0.5f, 0.50625f, 1, true));
    TEST_ASSERT_EQUAL_UINT8(10, s.steps());
    // Measured again over ten commands, downwards: the same.
    TEST_ASSERT_TRUE(s.observe(0.5f, 0.4375f, 10, true));
    TEST_ASSERT_EQUAL_UINT8(10, s.steps());
}

// Never more than the old fifteen, never fewer than one.
void test_the_count_is_bounded()
{
    AmsVolumeStep s;
    TEST_ASSERT_TRUE(s.observe(0.5f, 0.5001f, 1, true));
    TEST_ASSERT_EQUAL_UINT8(AmsVolumeStep::MAX_STEPS, s.steps());
    TEST_ASSERT_TRUE(s.observe(0.2f, 0.6f, 1, true));
    TEST_ASSERT_EQUAL_UINT8(1, s.steps());
}

// One silent burst (a late report) changes nothing; two in a row mean the
// phone does not report, and a press becomes four commands, not fifteen.
void test_two_silent_bursts_fall_back_to_four()
{
    AmsVolumeStep s;
    TEST_ASSERT_FALSE(s.observe(0.0f, 0.0f, 1, false));
    // 0 is an edge: not even a silent burst.
    TEST_ASSERT_EQUAL_UINT8(1, s.steps());
    TEST_ASSERT_FALSE(s.observe(0.5f, 0.5f, 1, false));
    TEST_ASSERT_EQUAL_UINT8(1, s.steps());
    TEST_ASSERT_FALSE(s.observe(0.5f, 0.5f, 1, false));
    TEST_ASSERT_EQUAL_UINT8(AmsVolumeStep::FALLBACK_STEPS, s.steps());
    TEST_ASSERT_FALSE(s.learned());
    // A report after all: measured, as usual.
    TEST_ASSERT_TRUE(s.observe(0.5f, 0.5625f, 1, true));
    TEST_ASSERT_EQUAL_UINT8(1, s.steps());
}

// Silent, measured, silent: the silences are not consecutive.
void test_a_measurement_resets_the_silence()
{
    AmsVolumeStep s;
    TEST_ASSERT_FALSE(s.observe(0.5f, 0.5f, 1, false));
    TEST_ASSERT_TRUE(s.observe(0.5f, 0.5625f, 1, true));
    TEST_ASSERT_FALSE(s.observe(0.5f, 0.5f, 1, false));
    TEST_ASSERT_EQUAL_UINT8(1, s.steps());
}

// Once learned, a burst that says nothing (no report in time) keeps it.
void test_a_learned_count_survives_a_silent_burst()
{
    AmsVolumeStep s;
    TEST_ASSERT_TRUE(s.observe(0.5f, 0.5125f, 1, true));
    TEST_ASSERT_EQUAL_UINT8(5, s.steps());
    TEST_ASSERT_FALSE(s.observe(0.5f, 0.5f, 5, false));
    TEST_ASSERT_EQUAL_UINT8(5, s.steps());
}

// A burst that ends at 0 or 1 was cut short by the end of the range.
void test_a_burst_into_the_end_stop_is_not_learned()
{
    AmsVolumeStep s;
    TEST_ASSERT_TRUE(s.observe(0.5f, 0.5125f, 1, true));
    TEST_ASSERT_FALSE(s.observe(0.9f, 1.0f, 5, true));
    TEST_ASSERT_FALSE(s.observe(0.0f, 0.0125f, 5, true));
    TEST_ASSERT_EQUAL_UINT8(5, s.steps());
    TEST_ASSERT_FALSE(s.observe(0.5f, 0.6f, 0, true));   // no writes: nothing
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_it_starts_at_one_command);
    RUN_TEST(test_a_button_sized_step_is_one_command);
    RUN_TEST(test_a_small_step_scales_up);
    RUN_TEST(test_the_count_is_bounded);
    RUN_TEST(test_two_silent_bursts_fall_back_to_four);
    RUN_TEST(test_a_measurement_resets_the_silence);
    RUN_TEST(test_a_learned_count_survives_a_silent_burst);
    RUN_TEST(test_a_burst_into_the_end_stop_is_not_learned);
    return UNITY_END();
}
