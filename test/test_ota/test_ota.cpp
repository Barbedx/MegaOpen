#include <unity.h>
#include "app/OtaVerdict.h"
#include "app/Presence.h"

// The OTA trial's verdict (src/app/OtaVerdict.h). A wrong one either keeps a
// build that hangs the keys, or throws away a good one on a bench with no
// panel, so each rule is pinned here with numbers.

void setUp() {}
void tearDown() {}

using Act = OtaVerdict::Act;
static constexpr uint32_t C = OtaVerdict::CONFIRM_MS;
static constexpr uint32_t T = OtaVerdict::TRIAL_MS;

// Every 20 ms from `from` to `to` (inclusive), all healthy or all not: the
// first verdict, and when it came (0 = none).
static Act run(OtaVerdict& v, uint32_t from, uint32_t to, bool healthy, uint32_t* at = nullptr)
{
    for (uint32_t t = from; t <= to; t += 20) {
        const Act a = v.tick(t, healthy);
        if (a != Act::None) { if (at) *at = t; return a; }
    }
    if (at) *at = 0;
    return Act::None;
}

void test_an_image_that_is_not_on_trial_gets_no_verdict()
{
    OtaVerdict v;
    v.begin(false);
    TEST_ASSERT_FALSE(v.onTrial());
    TEST_ASSERT_TRUE(run(v, 0, T + 60000, true) == Act::None);
    TEST_ASSERT_TRUE(run(v, T + 60020, 2 * T, false) == Act::None);
}

void test_confirmed_after_confirm_ms_healthy()
{
    OtaVerdict v;
    v.begin(true);
    TEST_ASSERT_TRUE(v.onTrial());
    uint32_t at = 0;
    TEST_ASSERT_TRUE(run(v, 1000, T, true, &at) == Act::Confirm);
    TEST_ASSERT_EQUAL_UINT32(1000 + C, at);
    TEST_ASSERT_FALSE(v.onTrial());
}

void test_healthy_from_boot_counts_from_the_first_sample()
{
    OtaVerdict v;
    v.begin(true);
    uint32_t at = 0;
    TEST_ASSERT_TRUE(run(v, 0, T, true, &at) == Act::Confirm);
    // The first sample at t=0 starts the count; it does not confirm at once.
    TEST_ASSERT_EQUAL_UINT32(C, at);
}

void test_a_break_starts_the_count_again()
{
    OtaVerdict v;
    v.begin(true);
    TEST_ASSERT_TRUE(run(v, 1000, 50000, true) == Act::None);
    TEST_ASSERT_TRUE(v.tick(50020, false) == Act::None);
    uint32_t at = 0;
    TEST_ASSERT_TRUE(run(v, 50040, T, true, &at) == Act::Confirm);
    TEST_ASSERT_EQUAL_UINT32(50040 + C, at);
}

void test_reverted_after_trial_ms_without_confirmation()
{
    OtaVerdict v;
    v.begin(true);
    uint32_t at = 0;
    TEST_ASSERT_TRUE(run(v, 0, T + 1000, false, &at) == Act::Revert);
    TEST_ASSERT_EQUAL_UINT32(T + 20, at);
    TEST_ASSERT_FALSE(v.onTrial());
}

void test_healthy_too_late_is_still_reverted()
{
    OtaVerdict v;
    v.begin(true);
    TEST_ASSERT_TRUE(run(v, 0, T - C + 20000, false) == Act::None);
    uint32_t at = 0;
    // Healthy for less than CONFIRM_MS when the trial runs out.
    TEST_ASSERT_TRUE(run(v, T - C + 20020, T + 1000, true, &at) == Act::Revert);
    TEST_ASSERT_EQUAL_UINT32(T + 20, at);
}

void test_one_verdict_per_boot()
{
    OtaVerdict v;
    v.begin(true);
    TEST_ASSERT_TRUE(run(v, 0, T, true) == Act::Confirm);
    TEST_ASSERT_TRUE(run(v, T, 2 * T, false) == Act::None);

    OtaVerdict w;
    w.begin(true);
    TEST_ASSERT_TRUE(run(w, 0, T + 20, false) == Act::Revert);
    TEST_ASSERT_TRUE(run(w, T + 40, 2 * T, true) == Act::None);
}

// ---- physical presence (app/Presence.h) -------------------------------------

// Never pressed: shut, whatever the time.
void test_presence_is_shut_until_a_press()
{
    App::Presence p;
    TEST_ASSERT_FALSE(p.allowed(0));
    TEST_ASSERT_FALSE(p.allowed(600000));
    TEST_ASSERT_EQUAL_UINT32(0, p.remainingMs(1000));
}

// A press opens WINDOW_MS, to the millisecond; a second press restarts it.
void test_a_press_opens_the_window_for_two_minutes()
{
    App::Presence p;
    p.press(5000);
    TEST_ASSERT_TRUE(p.allowed(5000));
    TEST_ASSERT_TRUE(p.allowed(5000 + App::Presence::WINDOW_MS - 1));
    TEST_ASSERT_FALSE(p.allowed(5000 + App::Presence::WINDOW_MS));
    TEST_ASSERT_EQUAL_UINT32(App::Presence::WINDOW_MS - 1000, p.remainingMs(6000));
    p.press(200000);
    TEST_ASSERT_TRUE(p.allowed(200000 + App::Presence::WINDOW_MS - 1));
    TEST_ASSERT_EQUAL_UINT32(2, p.presses());
}

// Across a millis() wrap.
void test_the_presence_window_survives_a_millis_wrap()
{
    App::Presence p;
    p.press(0xFFFFF000u);
    TEST_ASSERT_TRUE(p.allowed(0x00001000u));
    TEST_ASSERT_FALSE(p.allowed(0xFFFFF000u + App::Presence::WINDOW_MS));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_image_that_is_not_on_trial_gets_no_verdict);
    RUN_TEST(test_confirmed_after_confirm_ms_healthy);
    RUN_TEST(test_healthy_from_boot_counts_from_the_first_sample);
    RUN_TEST(test_a_break_starts_the_count_again);
    RUN_TEST(test_reverted_after_trial_ms_without_confirmation);
    RUN_TEST(test_healthy_too_late_is_still_reverted);
    RUN_TEST(test_one_verdict_per_boot);
    RUN_TEST(test_presence_is_shut_until_a_press);
    RUN_TEST(test_a_press_opens_the_window_for_two_minutes);
    RUN_TEST(test_the_presence_window_survives_a_millis_wrap);
    return UNITY_END();
}
