#include <unity.h>
#include "vehicle/CanSniffer.h"

using namespace Vehicle;

void setUp() {}
void tearDown() {}

static void feed(CanSniffer& s, uint32_t id, uint8_t b0, uint8_t b1, uint32_t t)
{
    const uint8_t d[2] = {b0, b1};
    s.ingest(id, d, 2, t);
}

void test_first_sighting_is_a_baseline_not_a_change()
{
    CanSniffer s;
    feed(s, 0x60D, 0x11, 0x22, 100);
    const auto* e = s.find(0x60D);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(0, e->changed);   // nothing to compare against yet
    TEST_ASSERT_EQUAL_UINT32(1, e->count);
}

void test_changed_mask_marks_only_the_bytes_that_moved()
{
    CanSniffer s;
    feed(s, 0x60D, 0x11, 0x22, 100);
    feed(s, 0x60D, 0x11, 0x23, 200);          // only byte 1 moved
    const auto* e = s.find(0x60D);
    TEST_ASSERT_EQUAL_UINT8(0b10, e->changed);
    TEST_ASSERT_EQUAL_UINT8(0x23, e->last[1]);
    TEST_ASSERT_EQUAL_UINT32(2, e->count);
}

void test_baseline_clears_history_but_keeps_the_rows()
{
    CanSniffer s;
    feed(s, 0x60D, 0x11, 0x22, 100);
    feed(s, 0x60D, 0x99, 0x22, 200);
    TEST_ASSERT_EQUAL_UINT8(0b01, s.find(0x60D)->changed);

    // "Mark baseline, then open the door" — the whole workflow depends on the row
    // surviving so the count and the last value stay meaningful.
    s.markBaseline();
    TEST_ASSERT_EQUAL_UINT8(0, s.find(0x60D)->changed);
    TEST_ASSERT_EQUAL_UINT8(1, s.count());
    TEST_ASSERT_EQUAL_UINT32(2, s.find(0x60D)->count);

    feed(s, 0x60D, 0x99, 0x77, 300);
    TEST_ASSERT_EQUAL_UINT8(0b10, s.find(0x60D)->changed);
}

void test_separate_ids_do_not_bleed_into_each_other()
{
    CanSniffer s;
    feed(s, 0x181, 0x01, 0x02, 10);
    feed(s, 0x354, 0x01, 0x02, 11);
    feed(s, 0x181, 0xFF, 0x02, 12);
    TEST_ASSERT_EQUAL_UINT8(2, s.count());
    TEST_ASSERT_EQUAL_UINT8(0b01, s.find(0x181)->changed);
    TEST_ASSERT_EQUAL_UINT8(0,    s.find(0x354)->changed);
}

void test_overflow_is_counted_rather_than_hidden()
{
    CanSniffer s;
    for (uint32_t i = 0; i < CanSniffer::MAX_IDS + 5; ++i) feed(s, 0x100 + i, 0, 0, i);
    TEST_ASSERT_EQUAL_UINT8(CanSniffer::MAX_IDS, s.count());
    // A table that silently drops IDs reads as "that signal isn't on this bus",
    // which is the one wrong conclusion this tool must never cause.
    TEST_ASSERT_EQUAL_UINT32(5, s.overflow());
}

void test_reset_forgets_everything()
{
    CanSniffer s;
    feed(s, 0x60D, 1, 2, 5);
    s.reset();
    TEST_ASSERT_EQUAL_UINT8(0, s.count());
    TEST_ASSERT_EQUAL_UINT32(0, s.frames());
    TEST_ASSERT_NULL(s.find(0x60D));
}

void test_off_ingests_nothing_and_on_keeps_the_table()
{
    CanSniffer s;
    feed(s, 0x60D, 1, 2, 5);
    s.setEnabled(false);
    feed(s, 0x60D, 9, 9, 6);       // would mark both bytes changed
    feed(s, 0x181, 1, 1, 7);       // would add a row
    TEST_ASSERT_EQUAL_UINT8(1, s.count());
    TEST_ASSERT_EQUAL_UINT32(1, s.frames());
    TEST_ASSERT_EQUAL_UINT8(0, s.find(0x60D)->changed);
    s.setEnabled(true);
    feed(s, 0x181, 1, 1, 8);
    TEST_ASSERT_EQUAL_UINT8(2, s.count());
    TEST_ASSERT_EQUAL_UINT32(2, s.frames());
}

void test_requests_still_land_while_off()
{
    CanSniffer s;
    feed(s, 0x60D, 1, 2, 5);
    s.setEnabled(false);
    s.requestReset();
    s.applyRequests();
    TEST_ASSERT_EQUAL_UINT8(0, s.count());
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_sighting_is_a_baseline_not_a_change);
    RUN_TEST(test_changed_mask_marks_only_the_bytes_that_moved);
    RUN_TEST(test_baseline_clears_history_but_keeps_the_rows);
    RUN_TEST(test_separate_ids_do_not_bleed_into_each_other);
    RUN_TEST(test_overflow_is_counted_rather_than_hidden);
    RUN_TEST(test_reset_forgets_everything);
    RUN_TEST(test_off_ingests_nothing_and_on_keeps_the_table);
    RUN_TEST(test_requests_still_land_while_off);
    return UNITY_END();
}
