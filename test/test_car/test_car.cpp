#include <unity.h>
#include "bus/Autobaud.h"
#include "bus/TwaiVehicleSource.h"

using Bus::Autobaud;
using Step = Autobaud::Step;
using State = Autobaud::State;

void setUp() {}
void tearDown() {}

// ---- the rate search ----------------------------------------------------------

void test_order_is_500_250_125_then_1M()
{
    TEST_ASSERT_EQUAL_UINT32(500000, Autobaud::rateAt(0));
    TEST_ASSERT_EQUAL_UINT32(250000, Autobaud::rateAt(1));
    TEST_ASSERT_EQUAL_UINT32(125000, Autobaud::rateAt(2));
    TEST_ASSERT_EQUAL_UINT32(1000000, Autobaud::rateAt(3));
    TEST_ASSERT_FALSE(Autobaud::valid(83333));
    TEST_ASSERT_FALSE(Autobaud::valid(0));
}

void test_no_bus_cycles_every_rate_after_its_dwell()
{
    Autobaud a;
    a.begin(0, 0, 0, 1500);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)State::Searching, (uint8_t)a.state());
    TEST_ASSERT_EQUAL_UINT32(500000, a.rate());
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Stay, (uint8_t)a.observe(1499, 0, 0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Next, (uint8_t)a.observe(1500, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(250000, a.rate());
    a.started(1500);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Next, (uint8_t)a.observe(3000, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(125000, a.rate());
    a.started(3000);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Next, (uint8_t)a.observe(4500, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(1000000, a.rate());
    TEST_ASSERT_EQUAL_UINT32(0, a.cycles());
    a.started(4500);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Next, (uint8_t)a.observe(6000, 0, 0));
    // Round again, for ever, counted.
    TEST_ASSERT_EQUAL_UINT32(500000, a.rate());
    TEST_ASSERT_EQUAL_UINT32(1, a.cycles());
    TEST_ASSERT_EQUAL_UINT32(0, a.detected());
}

void test_clean_frames_lock_the_first_rate_that_has_them()
{
    Autobaud a;
    a.begin(0, 0, 0);
    a.observe(1500, 0, 0);                    // 500k: nothing
    a.started(1500);
    TEST_ASSERT_EQUAL_UINT32(250000, a.rate());
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Stay, (uint8_t)a.observe(1600, 3, 0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Lock, (uint8_t)a.observe(1700, Autobaud::LOCK_FRAMES, 0));
    TEST_ASSERT_TRUE(a.locked());
    TEST_ASSERT_EQUAL_UINT32(250000, a.detected());
    // Locked stays locked: a sleeping car is a quiet bus at the right rate,
    // and a burst of noise is not a reason to search again.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Stay, (uint8_t)a.observe(999999, 0, 5));
    TEST_ASSERT_EQUAL_UINT32(250000, a.rate());
}

void test_error_frames_reject_a_rate_at_once()
{
    Autobaud a;
    a.begin(0, 0, 0);
    // Frames AND errors: the errors win, whatever the frame count.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Next, (uint8_t)a.observe(100, 50, 1));
    TEST_ASSERT_EQUAL_UINT32(250000, a.rate());
    TEST_ASSERT_EQUAL_UINT32(1, a.rejected());
    TEST_ASSERT_FALSE(a.locked());
}

void test_a_sparse_bus_locks_at_the_end_of_the_dwell()
{
    Autobaud a;
    a.begin(0, 0, 0, 1500);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Stay, (uint8_t)a.observe(1000, 1, 0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Lock, (uint8_t)a.observe(1500, 1, 0));
    TEST_ASSERT_EQUAL_UINT32(500000, a.detected());
}

void test_saved_rate_is_tried_first_then_the_rest_in_order()
{
    Autobaud a;
    a.begin(0, 125000, 0, 1000);
    TEST_ASSERT_EQUAL_UINT32(125000, a.rate());
    a.observe(1000, 0, 0); a.started(1000);
    TEST_ASSERT_EQUAL_UINT32(500000, a.rate());
    a.observe(2000, 0, 0); a.started(2000);
    TEST_ASSERT_EQUAL_UINT32(250000, a.rate());
    a.observe(3000, 0, 0); a.started(3000);
    TEST_ASSERT_EQUAL_UINT32(1000000, a.rate());   // 125k is not tried twice
    a.observe(4000, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(125000, a.rate());
    TEST_ASSERT_EQUAL_UINT32(1, a.cycles());
}

void test_invalid_saved_rate_is_ignored()
{
    Autobaud a;
    a.begin(0, 33333, 0);
    TEST_ASSERT_EQUAL_UINT32(500000, a.rate());
}

void test_manual_rate_wins_and_never_cycles()
{
    Autobaud a;
    a.begin(1000000, 500000, 0, 1000);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)State::Manual, (uint8_t)a.state());
    TEST_ASSERT_EQUAL_UINT32(1000000, a.rate());
    // Errors, silence, frames: none of it moves a hand-set rate.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Stay, (uint8_t)a.observe(50000, 0, 9));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Stay, (uint8_t)a.observe(90000, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(1000000, a.rate());
    TEST_ASSERT_EQUAL_UINT32(0, a.detected());     // set, not detected: nothing to save
}

void test_invalid_manual_rate_means_auto()
{
    Autobaud a;
    a.begin(42, 0, 0);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)State::Searching, (uint8_t)a.state());
}

void test_millis_wrap_does_not_end_a_dwell_early()
{
    Autobaud a;
    a.begin(0, 0, 0xFFFFFF00u, 1500);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Stay, (uint8_t)a.observe(0x00000100u, 0, 0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Step::Next, (uint8_t)a.observe(0xFFFFFF00u + 1500u, 0, 0));
}

// ---- the TWAI hand-over ring ------------------------------------------------------

void test_twai_ring_hands_frames_over_in_order_and_counts_overflow()
{
    static TwaiVehicleSource src;   // big: off the stack
    Frame f;
    f.len = 1;
    for (uint16_t i = 0; i < TwaiVehicleSource::DEPTH + 5; ++i) {
        f.id = i;
        src.put(f);
    }
    // One slot is the full/empty gap.
    Frame out[TwaiVehicleSource::DEPTH];
    const size_t n = src.drain(out, TwaiVehicleSource::DEPTH, 0);
    TEST_ASSERT_EQUAL_UINT32(TwaiVehicleSource::DEPTH - 1, n);
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_EQUAL_UINT32(i, out[i].id);
        TEST_ASSERT_EQUAL_UINT8(Frame::SRC_VH_CAN, out[i].source);
    }
    TEST_ASSERT_EQUAL_UINT32(6, src.stats().dropped);
    TEST_ASSERT_EQUAL_UINT32(n, src.stats().frames);
    TEST_ASSERT_EQUAL_UINT32(0, src.drain(out, 4, 0));
}

void test_twai_ring_drain_respects_max()
{
    static TwaiVehicleSource src;
    Frame f;
    for (uint8_t i = 0; i < 10; ++i) { f.id = i; src.put(f); }
    Frame out[4];
    TEST_ASSERT_EQUAL_UINT32(4, src.drain(out, 4, 0));
    TEST_ASSERT_EQUAL_UINT32(3, out[3].id);
    TEST_ASSERT_EQUAL_UINT32(4, src.drain(out, 4, 0));
    TEST_ASSERT_EQUAL_UINT32(2, src.drain(out, 4, 0));
    TEST_ASSERT_EQUAL_UINT32(9, out[1].id);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_order_is_500_250_125_then_1M);
    RUN_TEST(test_no_bus_cycles_every_rate_after_its_dwell);
    RUN_TEST(test_clean_frames_lock_the_first_rate_that_has_them);
    RUN_TEST(test_error_frames_reject_a_rate_at_once);
    RUN_TEST(test_a_sparse_bus_locks_at_the_end_of_the_dwell);
    RUN_TEST(test_saved_rate_is_tried_first_then_the_rest_in_order);
    RUN_TEST(test_invalid_saved_rate_is_ignored);
    RUN_TEST(test_manual_rate_wins_and_never_cycles);
    RUN_TEST(test_invalid_manual_rate_means_auto);
    RUN_TEST(test_millis_wrap_does_not_end_a_dwell_early);
    RUN_TEST(test_twai_ring_hands_frames_over_in_order_and_counts_overflow);
    RUN_TEST(test_twai_ring_drain_respects_max);
    return UNITY_END();
}
