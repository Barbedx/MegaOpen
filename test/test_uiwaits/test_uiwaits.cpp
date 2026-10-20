#include <unity.h>
#include "app/UiWaits.h"

using namespace UiWaits;

void setUp() {}
void tearDown() {}

// The ui task's table of renders still owed a verdict (src/app/UiWaits.h).
// Every rule here guards one way the screen could stop for good: a clock retry
// waiting on a ticket that will never answer, or a verdict credited twice.

static void test_a_wait_is_answered_once()
{
    Waits w;
    uint8_t tag = 0;
    TEST_ASSERT_TRUE(w.add(11, 3, 1000));
    TEST_ASSERT_TRUE(w.take(11, tag));
    TEST_ASSERT_EQUAL_UINT8(3, tag);
    TEST_ASSERT_FALSE(w.take(11, tag));
    TEST_ASSERT_EQUAL_UINT8(0, w.used());
}

static void test_a_wait_times_out_even_across_millis_wrap()
{
    Waits w;
    Ticket t = 0;
    uint8_t tag = 0;
    const uint32_t t0 = 0xFFFFF000u;               // millis() about to wrap
    TEST_ASSERT_TRUE(w.add(12, 2, t0));
    TEST_ASSERT_FALSE(w.expire(t0 + Waits::TIMEOUT_MS - 1, t, tag));
    TEST_ASSERT_TRUE(w.expire(t0 + Waits::TIMEOUT_MS, t, tag));
    TEST_ASSERT_EQUAL_UINT16(12, t);
    TEST_ASSERT_EQUAL_UINT8(2, tag);
    TEST_ASSERT_EQUAL_UINT32(1, w.timeouts());
    // A late verdict for it is ignored.
    TEST_ASSERT_FALSE(w.take(12, tag));
}

static void test_a_full_wait_table_refuses_and_counts()
{
    Waits w;
    for (uint8_t i = 0; i < Waits::CAP; ++i) TEST_ASSERT_TRUE(w.add(i + 1, 0, 0));
    TEST_ASSERT_FALSE(w.add(99, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(1, w.full());
    TEST_ASSERT_FALSE(w.add(NO_TICKET, 0, 0));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_wait_is_answered_once);
    RUN_TEST(test_a_wait_times_out_even_across_millis_wrap);
    RUN_TEST(test_a_full_wait_table_refuses_and_counts);
    return UNITY_END();
}
