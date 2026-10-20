#include <unity.h>
#include "app/MailboxIds.h"

using namespace MailboxIds;

void setUp() {}
void tearDown() {}

// The ticket <-> request protocol between the ui task and the panel task
// (src/app/MailboxIds.h). Every rule here guards one way the screen could stop
// for good: a pane slot never handed back, a clock retry waiting on an id that
// will never answer, or a verdict credited to the wrong request.

// ---- ids ---------------------------------------------------------------------
static void test_ids_are_never_zero_even_across_the_wrap()
{
    IdGen g;
    ReqId prev = g.next();
    TEST_ASSERT_NOT_EQUAL(NO_REQ, prev);
    for (uint32_t i = 0; i < 70000; ++i) {
        const ReqId id = g.next();
        TEST_ASSERT_NOT_EQUAL(NO_REQ, id);
        TEST_ASSERT_NOT_EQUAL(prev, id);
        prev = id;
    }
}

// ---- the panel's half ----------------------------------------------------------
static void test_an_accepted_render_is_mapped_and_completes_later()
{
    TicketMap m;
    Completion c;
    m.beforeRender();
    TEST_ASSERT_FALSE(m.afterRender(7, 1, true, 0, /*before*/ 3, /*after*/ 4, c));
    TEST_ASSERT_EQUAL_UINT8(1, m.used());
    TEST_ASSERT_TRUE(m.complete(4, 0, c));
    TEST_ASSERT_EQUAL_UINT16(7, c.id);
    TEST_ASSERT_EQUAL_UINT8(1, c.slot);
    TEST_ASSERT_FALSE(c.synth);
    TEST_ASSERT_EQUAL_UINT8(0, m.used());
}

static void test_a_refused_render_synthesises_and_hands_the_slot_back()
{
    TicketMap m;
    Completion c;
    m.beforeRender();
    // kNoTicket: BadArgument / TooLong.
    TEST_ASSERT_TRUE(m.afterRender(9, 0, false, 5, 3, NO_TICKET, c));
    TEST_ASSERT_EQUAL_UINT16(9, c.id);
    TEST_ASSERT_EQUAL_UINT8(0, c.slot);
    TEST_ASSERT_EQUAL_UINT8(5, c.result);
    TEST_ASSERT_TRUE(c.synth);
    TEST_ASSERT_EQUAL_UINT8(0, m.used());
    TEST_ASSERT_EQUAL_UINT32(1, m.synthesised());
}

static void test_ok_with_nothing_enqueued_synthesises()
{
    TicketMap m;
    Completion c;
    m.beforeRender();
    TEST_ASSERT_TRUE(m.afterRender(10, 1, true, 0, 4, 4, c));
    TEST_ASSERT_TRUE(c.synth);
    TEST_ASSERT_EQUAL_UINT8(1, c.slot);
    TEST_ASSERT_EQUAL_UINT8(0, m.used());
}

static void test_a_coalesced_abort_maps_to_the_older_request()
{
    TicketMap m;
    Completion c;
    m.beforeRender();
    TEST_ASSERT_FALSE(m.afterRender(1, 0, true, 0, 0, 10, c));
    // The newer render aborts ticket 10 from inside its own call...
    m.beforeRender();
    const uint8_t ABORTED = 7;
    TEST_ASSERT_TRUE(m.complete(10, ABORTED, c));
    TEST_ASSERT_EQUAL_UINT16(1, c.id);
    TEST_ASSERT_EQUAL_UINT8(0, c.slot);
    TEST_ASSERT_EQUAL_UINT8(ABORTED, c.result);
    // ...and only then is its own ticket mapped.
    TEST_ASSERT_FALSE(m.afterRender(2, 1, true, 0, 10, 11, c));
    TEST_ASSERT_TRUE(m.complete(11, 0, c));
    TEST_ASSERT_EQUAL_UINT16(2, c.id);
    TEST_ASSERT_EQUAL_UINT8(1, c.slot);
}

static void test_a_ticket_completed_inside_its_own_call_is_not_left_mapped()
{
    TicketMap m;
    Completion c;
    m.beforeRender();
    TEST_ASSERT_FALSE(m.complete(20, 0, c));      // nobody knows it yet
    TEST_ASSERT_TRUE(m.afterRender(3, 1, true, 0, 19, 20, c));
    TEST_ASSERT_EQUAL_UINT16(3, c.id);
    TEST_ASSERT_EQUAL_UINT8(1, c.slot);
    TEST_ASSERT_EQUAL_UINT8(0, m.used());
}

static void test_an_early_verdict_from_a_previous_call_does_not_leak()
{
    TicketMap m;
    Completion c;
    TEST_ASSERT_FALSE(m.complete(30, 0, c));      // an untracked render's verdict
    m.beforeRender();                              // forgets it
    TEST_ASSERT_FALSE(m.afterRender(4, 0, true, 0, 29, 30, c));
    TEST_ASSERT_EQUAL_UINT8(1, m.used());
}

static void test_a_full_table_hands_the_slot_back_now()
{
    TicketMap m;
    Completion c;
    for (uint8_t i = 0; i < TicketMap::CAP; ++i) {
        m.beforeRender();
        TEST_ASSERT_FALSE(m.afterRender(100 + i, NO_SLOT, true, 0, i, i + 1, c));
    }
    m.beforeRender();
    TEST_ASSERT_TRUE(m.afterRender(200, 1, true, 0, 50, 51, c));
    TEST_ASSERT_TRUE(c.synth);
    TEST_ASSERT_EQUAL_UINT8(1, c.slot);
    TEST_ASSERT_EQUAL_UINT32(1, m.full());
}

static void test_dropped_is_always_synthesised_with_its_slot()
{
    const Completion c = TicketMap::dropped(5, 1, 9);
    TEST_ASSERT_EQUAL_UINT16(5, c.id);
    TEST_ASSERT_EQUAL_UINT8(1, c.slot);
    TEST_ASSERT_TRUE(c.synth);
}

// ---- the ui's half -------------------------------------------------------------
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

static void test_a_wait_times_out_after_four_seconds_even_across_millis_wrap()
{
    Waits w;
    ReqId id = 0;
    uint8_t tag = 0;
    const uint32_t t0 = 0xFFFFF000u;               // millis() about to wrap
    TEST_ASSERT_TRUE(w.add(12, 2, t0));
    TEST_ASSERT_FALSE(w.expire(t0 + Waits::TIMEOUT_MS - 1, id, tag));
    TEST_ASSERT_TRUE(w.expire(t0 + Waits::TIMEOUT_MS, id, tag));
    TEST_ASSERT_EQUAL_UINT16(12, id);
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
    TEST_ASSERT_FALSE(w.add(NO_REQ, 0, 0));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_ids_are_never_zero_even_across_the_wrap);
    RUN_TEST(test_an_accepted_render_is_mapped_and_completes_later);
    RUN_TEST(test_a_refused_render_synthesises_and_hands_the_slot_back);
    RUN_TEST(test_ok_with_nothing_enqueued_synthesises);
    RUN_TEST(test_a_coalesced_abort_maps_to_the_older_request);
    RUN_TEST(test_a_ticket_completed_inside_its_own_call_is_not_left_mapped);
    RUN_TEST(test_an_early_verdict_from_a_previous_call_does_not_leak);
    RUN_TEST(test_a_full_table_hands_the_slot_back_now);
    RUN_TEST(test_dropped_is_always_synthesised_with_its_slot);
    RUN_TEST(test_a_wait_is_answered_once);
    RUN_TEST(test_a_wait_times_out_after_four_seconds_even_across_millis_wrap);
    RUN_TEST(test_a_full_wait_table_refuses_and_counts);
    return UNITY_END();
}
