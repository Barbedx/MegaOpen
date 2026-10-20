#include <unity.h>
#include <string.h>
#include "utils/Snapshot.h"
#include "utils/Staged.h"
#include "utils/LatencyHist.h"
#include "app/RequestReply.h"
#include "app/TaskAge.h"
#include "app/OneShot.h"

// The runtime scaffolding of docs/design/event-tasks.md, Step 1. Every one of
// these is a promise another task relies on without being able to check it:
// a snapshot that is never half-written, an owner that never waits on httpd, a
// p99 that is never optimistic, and a web request that, once it gave up, can
// never be answered into a slot somebody else now holds.

void setUp() {}
void tearDown() {}

// ---- Snapshot ------------------------------------------------------------------
struct Status { uint32_t a = 0; uint32_t b = 0; char text[16] = {0}; };

void test_a_snapshot_reads_what_was_published()
{
    Snapshot<Status> s;
    Status in;
    in.a = 7; in.b = 9; strcpy(in.text, "ready");
    s.publish(in);
    Status out;
    s.read(out);
    TEST_ASSERT_EQUAL_UINT32(7, out.a);
    TEST_ASSERT_EQUAL_UINT32(9, out.b);
    TEST_ASSERT_EQUAL_STRING("ready", out.text);
}

void test_a_snapshot_counts_publishes_so_a_reader_can_skip_old_news()
{
    Snapshot<Status> s;
    TEST_ASSERT_EQUAL_UINT32(0, s.seq());
    Status v;
    s.publish(v);
    s.publish(v);
    Status out;
    TEST_ASSERT_EQUAL_UINT32(2, s.read(out));
    TEST_ASSERT_EQUAL_UINT32(2, s.seq());
}

void test_an_unpublished_snapshot_reads_as_zero()
{
    Snapshot<Status> s;
    const Status out = s.read();
    TEST_ASSERT_EQUAL_UINT32(0, out.a);
    TEST_ASSERT_EQUAL_STRING("", out.text);
}

// ---- Staged --------------------------------------------------------------------
struct Cfg { uint32_t mask = 0; uint16_t pulse = 0; };

void test_staged_changes_nothing_live_until_the_owner_applies()
{
    Staged<Cfg> s;
    Cfg c; c.mask = 1; c.pulse = 100;
    s.init(c);
    Cfg n = c; n.mask = 0xF;
    TEST_ASSERT_TRUE(s.stage(n));
    TEST_ASSERT_TRUE(s.dirty());
    TEST_ASSERT_EQUAL_UINT32(1, s.live().mask);
    TEST_ASSERT_TRUE(s.applyIfDirty());
    TEST_ASSERT_EQUAL_UINT32(0xF, s.live().mask);
    TEST_ASSERT_FALSE(s.dirty());
}

void test_staged_apply_is_a_buffer_swap_not_a_copy()
{
    Staged<Cfg> s;
    s.init(Cfg{});
    const Cfg* before = &s.live();
    TEST_ASSERT_TRUE(s.edit([](Cfg& c) { c.pulse = 250; }));
    TEST_ASSERT_TRUE(s.applyIfDirty());
    const Cfg* after = &s.live();
    // The owner now reads the other buffer: nothing was copied on its side.
    TEST_ASSERT_TRUE(before != after);
    TEST_ASSERT_EQUAL_UINT16(250, s.live().pulse);
    // And the next edit lands in the buffer the owner just let go of.
    TEST_ASSERT_TRUE(s.edit([](Cfg& c) { c.pulse = 300; }));
    TEST_ASSERT_TRUE(s.applyIfDirty());
    TEST_ASSERT_TRUE(&s.live() == before);
    TEST_ASSERT_EQUAL_UINT16(300, s.live().pulse);
}

void test_staged_nothing_to_apply_is_a_cheap_no()
{
    Staged<Cfg> s;
    s.init(Cfg{});
    TEST_ASSERT_FALSE(s.applyIfDirty());
    TEST_ASSERT_EQUAL_UINT32(0, s.applied());
    TEST_ASSERT_EQUAL_UINT32(0, s.busySkips());
}

// The owner's apply runs while a writer holds the mutex — here, from inside the
// writer's own edit. It must come back at once with "not now", leave the live
// value alone, and pick the change up on the next call.
void test_staged_owner_never_waits_for_a_writer()
{
    Staged<Cfg> s;
    s.init(Cfg{});
    TEST_ASSERT_TRUE(s.stage(Cfg{3, 30}));
    bool applied = true;
    TEST_ASSERT_TRUE(s.edit([&](Cfg& c) {
        c.pulse = 40;
        applied = s.applyIfDirty();     // the owner, mid-edit
    }));
    TEST_ASSERT_FALSE(applied);
    TEST_ASSERT_EQUAL_UINT32(1, s.busySkips());
    TEST_ASSERT_EQUAL_UINT32(0, s.live().mask);
    TEST_ASSERT_TRUE(s.applyIfDirty());
    TEST_ASSERT_EQUAL_UINT32(3, s.live().mask);
    TEST_ASSERT_EQUAL_UINT16(40, s.live().pulse);
}

// Two edits between applies both land: each starts from the pending value,
// not from the live one.
void test_staged_edits_accumulate_until_applied()
{
    Staged<Cfg> s;
    s.init(Cfg{});
    TEST_ASSERT_TRUE(s.edit([](Cfg& c) { c.mask = 5; }));
    TEST_ASSERT_TRUE(s.edit([](Cfg& c) { c.pulse = 60; }));
    Cfg peek;
    TEST_ASSERT_TRUE(s.peek(peek));
    TEST_ASSERT_EQUAL_UINT32(5, peek.mask);
    TEST_ASSERT_EQUAL_UINT16(60, peek.pulse);
    TEST_ASSERT_TRUE(s.applyIfDirty());
    TEST_ASSERT_EQUAL_UINT32(5, s.live().mask);
    TEST_ASSERT_EQUAL_UINT16(60, s.live().pulse);
}

void test_staged_writer_gives_up_rather_than_deadlock()
{
    Staged<Cfg> s;
    s.init(Cfg{});
    bool inner = true;
    TEST_ASSERT_TRUE(s.edit([&](Cfg&) { inner = s.stage(Cfg{9, 9}); }));
    TEST_ASSERT_FALSE(inner);
}

// ---- LatencyHist ---------------------------------------------------------------
void test_an_empty_histogram_reports_zeros()
{
    LatencyHist h;
    TEST_ASSERT_EQUAL_UINT32(0, h.count());
    TEST_ASSERT_EQUAL_UINT32(0, h.p50());
    TEST_ASSERT_EQUAL_UINT32(0, h.p99());
    TEST_ASSERT_EQUAL_UINT32(0, h.max());
}

void test_buckets_are_powers_of_two()
{
    LatencyHist h;
    h.record(0);      // bucket 0
    h.record(1);      // bucket 1
    h.record(2);      // bucket 2
    h.record(3);      // bucket 2
    h.record(4);      // bucket 3
    h.record(1023);   // bucket 10
    h.record(1024);   // bucket 11
    TEST_ASSERT_EQUAL_UINT32(1, h.bucket(0));
    TEST_ASSERT_EQUAL_UINT32(1, h.bucket(1));
    TEST_ASSERT_EQUAL_UINT32(2, h.bucket(2));
    TEST_ASSERT_EQUAL_UINT32(1, h.bucket(3));
    TEST_ASSERT_EQUAL_UINT32(1, h.bucket(10));
    TEST_ASSERT_EQUAL_UINT32(1, h.bucket(11));
    TEST_ASSERT_EQUAL_UINT32(7, h.count());
}

// 98 fast keys and 2 slow ones: p50 sits with the fast ones, p99 with the
// slow, and neither is ever below the true value.
void test_percentiles_are_never_optimistic()
{
    LatencyHist h;
    for (int i = 0; i < 98; ++i) h.record(900);    // bucket 10: up to 1023
    h.record(5000);                                  // bucket 13: up to 8191
    h.record(6000);
    TEST_ASSERT_EQUAL_UINT32(1023, h.p50());
    TEST_ASSERT_TRUE(h.p50() >= 900);
    TEST_ASSERT_TRUE(h.p99() >= 5000);
    // Clamped to the real maximum, not the bucket's 8191.
    TEST_ASSERT_EQUAL_UINT32(6000, h.p99());
    TEST_ASSERT_EQUAL_UINT32(6000, h.max());
}

void test_a_huge_value_lands_in_the_last_bucket()
{
    LatencyHist h;
    h.record(0xFFFFFFFFu);
    TEST_ASSERT_EQUAL_UINT32(1, h.bucket(LatencyHist::BUCKETS - 1));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, h.p99());
}

void test_reset_starts_a_new_window()
{
    LatencyHist h;
    h.record(100);
    h.reset();
    TEST_ASSERT_EQUAL_UINT32(0, h.count());
    TEST_ASSERT_EQUAL_UINT32(0, h.max());
    h.record(3);
    TEST_ASSERT_EQUAL_UINT32(3, h.p99());
}

// ---- Request/Reply -------------------------------------------------------------
static RR::Args args(uint8_t op, uint32_t a0)
{
    RR::Args a;
    a.op = op;
    a.u[0] = a0;
    return a;
}

void test_rr_a_request_round_trips()
{
    RR::Table t;
    RR::Ticket tk;
    TEST_ASSERT_TRUE(t.acquire(args(3, 42), tk));
    TEST_ASSERT_TRUE(tk.valid());

    RR::Args got;                                  // the owner
    TEST_ASSERT_TRUE(t.take(tk, got));
    TEST_ASSERT_EQUAL_UINT8(3, got.op);
    TEST_ASSERT_EQUAL_UINT32(42, got.u[0]);
    RR::Result r; r.status = 1; r.u[0] = 84;
    TEST_ASSERT_TRUE(t.complete(tk, r));

    RR::Result out;                                // the waiter
    TEST_ASSERT_TRUE(t.collect(tk, out));
    TEST_ASSERT_EQUAL_INT32(1, out.status);
    TEST_ASSERT_EQUAL_UINT32(84, out.u[0]);
    TEST_ASSERT_TRUE(t.state(tk.idx) == RR::State::Free);
}

void test_rr_generation_is_never_zero_and_moves_on_every_claim()
{
    RR::Table t;
    RR::Ticket a, b;
    TEST_ASSERT_TRUE(t.acquire(args(1, 0), a));
    TEST_ASSERT_TRUE(a.gen != 0);
    t.release(a);
    TEST_ASSERT_TRUE(t.acquire(args(1, 0), b));
    TEST_ASSERT_EQUAL_UINT8(a.idx, b.idx);
    TEST_ASSERT_TRUE(b.gen != a.gen);
}

void test_rr_all_slots_busy_is_a_refusal_not_a_wait()
{
    RR::Table t;
    RR::Ticket tk[RR::SLOTS + 1];
    for (uint8_t i = 0; i < RR::SLOTS; ++i) TEST_ASSERT_TRUE(t.acquire(args(1, i), tk[i]));
    TEST_ASSERT_FALSE(t.acquire(args(1, 99), tk[RR::SLOTS]));
}

// The waiter timed out before the owner got to it: the owner skips the work
// and frees the slot; nothing is written for a handler that has returned.
void test_rr_abandoned_before_the_owner_ran_is_skipped()
{
    RR::Table t;
    RR::Ticket tk;
    TEST_ASSERT_TRUE(t.acquire(args(5, 1), tk));
    TEST_ASSERT_TRUE(t.abandon(tk));
    TEST_ASSERT_TRUE(t.state(tk.idx) == RR::State::Abandoned);
    RR::Args got;
    TEST_ASSERT_FALSE(t.take(tk, got));
    TEST_ASSERT_TRUE(t.state(tk.idx) == RR::State::Free);
    TEST_ASSERT_EQUAL_UINT32(1, t.stale());
}

// The waiter timed out while the owner was working: the owner's answer is
// dropped, the slot is freed, and the owner is told not to signal.
void test_rr_abandoned_while_the_owner_ran_gets_no_answer()
{
    RR::Table t;
    RR::Ticket tk;
    TEST_ASSERT_TRUE(t.acquire(args(5, 1), tk));
    RR::Args got;
    TEST_ASSERT_TRUE(t.take(tk, got));
    TEST_ASSERT_TRUE(t.abandon(tk));
    RR::Result r; r.status = 7;
    TEST_ASSERT_FALSE(t.complete(tk, r));
    TEST_ASSERT_TRUE(t.state(tk.idx) == RR::State::Free);
}

// The owner finished just as the waiter timed out: abandon says "no, it is
// done", and the waiter collects the answer after all.
void test_rr_a_late_finish_is_still_collected()
{
    RR::Table t;
    RR::Ticket tk;
    TEST_ASSERT_TRUE(t.acquire(args(5, 1), tk));
    RR::Args got;
    TEST_ASSERT_TRUE(t.take(tk, got));
    RR::Result r; r.status = 3;
    TEST_ASSERT_TRUE(t.complete(tk, r));
    TEST_ASSERT_FALSE(t.abandon(tk));
    RR::Result out;
    TEST_ASSERT_TRUE(t.collect(tk, out));
    TEST_ASSERT_EQUAL_INT32(3, out.status);
}

// An abandoned slot is reused by the next request. The owner's message for
// the OLD request then arrives: its generation no longer matches, so it can
// neither run the old op nor overwrite the new request's answer.
void test_rr_a_reused_slot_ignores_the_old_generation()
{
    RR::Table t;
    RR::Ticket old;
    // Fill every slot so the reuse has to take the abandoned one.
    RR::Ticket busy[RR::SLOTS];
    TEST_ASSERT_TRUE(t.acquire(args(1, 1), old));
    for (uint8_t i = 1; i < RR::SLOTS; ++i) TEST_ASSERT_TRUE(t.acquire(args(2, i), busy[i]));
    TEST_ASSERT_TRUE(t.abandon(old));

    RR::Ticket fresh;
    TEST_ASSERT_TRUE(t.acquire(args(9, 2), fresh));
    TEST_ASSERT_EQUAL_UINT8(old.idx, fresh.idx);
    TEST_ASSERT_TRUE(fresh.gen != old.gen);

    RR::Args got;
    TEST_ASSERT_FALSE(t.take(old, got));                 // stale: not run
    RR::Result bogus; bogus.status = -1;
    TEST_ASSERT_FALSE(t.complete(old, bogus));           // stale: not written
    TEST_ASSERT_TRUE(t.state(fresh.idx) == RR::State::Pending);

    TEST_ASSERT_TRUE(t.take(fresh, got));
    TEST_ASSERT_EQUAL_UINT8(9, got.op);
    RR::Result r; r.status = 2;
    TEST_ASSERT_TRUE(t.complete(fresh, r));
    RR::Result out;
    TEST_ASSERT_TRUE(t.collect(fresh, out));
    TEST_ASSERT_EQUAL_INT32(2, out.status);
}

void test_rr_a_free_slot_is_preferred_over_an_abandoned_one()
{
    RR::Table t;
    RR::Ticket a, b;
    TEST_ASSERT_TRUE(t.acquire(args(1, 0), a));
    TEST_ASSERT_TRUE(t.abandon(a));
    TEST_ASSERT_TRUE(t.acquire(args(1, 0), b));
    // Slot a stays abandoned for its owner message to find and free.
    TEST_ASSERT_TRUE(b.idx != a.idx);
    TEST_ASSERT_TRUE(t.state(a.idx) == RR::State::Abandoned);
}

void test_rr_a_failed_post_frees_the_slot()
{
    RR::Table t;
    RR::Ticket tk;
    TEST_ASSERT_TRUE(t.acquire(args(1, 0), tk));
    t.release(tk);
    TEST_ASSERT_TRUE(t.state(tk.idx) == RR::State::Free);
}

void test_rr_an_empty_ticket_matches_nothing()
{
    RR::Table t;
    RR::Ticket none;
    RR::Args got;
    RR::Result r, out;
    TEST_ASSERT_FALSE(none.valid());
    TEST_ASSERT_FALSE(t.take(none, got));
    TEST_ASSERT_FALSE(t.complete(none, r));
    TEST_ASSERT_FALSE(t.collect(none, out));
}

// A stray give wakes the waiter before the owner has answered. The waiter
// must keep waiting; when the time then runs out the slot is abandoned, and
// the owner's eventual answer frees it rather than leaving it Done for good.
void test_rr_a_stray_wake_does_not_lose_the_slot()
{
    RR::Table t;
    RR::Ticket tk;
    TEST_ASSERT_TRUE(t.acquire(args(4, 1), tk));
    RR::Result out;
    TEST_ASSERT_TRUE(t.settle(tk, true, out) == RR::Table::Settle::KeepWaiting);
    TEST_ASSERT_TRUE(t.state(tk.idx) == RR::State::Pending);

    TEST_ASSERT_TRUE(t.settle(tk, false, out) == RR::Table::Settle::GaveUp);
    TEST_ASSERT_TRUE(t.state(tk.idx) == RR::State::Abandoned);

    RR::Result r; r.status = 5;
    TEST_ASSERT_FALSE(t.complete(tk, r));
    TEST_ASSERT_TRUE(t.state(tk.idx) == RR::State::Free);
}

// A stray wake, then the real answer: the next wait collects it.
void test_rr_a_stray_wake_then_the_answer_is_collected()
{
    RR::Table t;
    RR::Ticket tk;
    TEST_ASSERT_TRUE(t.acquire(args(4, 2), tk));
    RR::Result out;
    TEST_ASSERT_TRUE(t.settle(tk, true, out) == RR::Table::Settle::KeepWaiting);
    RR::Args got;
    TEST_ASSERT_TRUE(t.take(tk, got));
    RR::Result r; r.status = 6;
    TEST_ASSERT_TRUE(t.complete(tk, r));
    TEST_ASSERT_TRUE(t.settle(tk, true, out) == RR::Table::Settle::Answer);
    TEST_ASSERT_EQUAL_INT32(6, out.status);
    TEST_ASSERT_TRUE(t.state(tk.idx) == RR::State::Free);
}

// The answer landed just as the wait ran out (no wake): it is still taken.
void test_rr_settle_takes_an_answer_that_beat_the_timeout()
{
    RR::Table t;
    RR::Ticket tk;
    TEST_ASSERT_TRUE(t.acquire(args(4, 3), tk));
    RR::Result r; r.status = 8;
    TEST_ASSERT_TRUE(t.complete(tk, r));
    RR::Result out;
    TEST_ASSERT_TRUE(t.settle(tk, false, out) == RR::Table::Settle::Answer);
    TEST_ASSERT_EQUAL_INT32(8, out.status);
    TEST_ASSERT_TRUE(t.state(tk.idx) == RR::State::Free);
}

// ---- task liveness (the OTA verdict) ------------------------------------------
struct FakeTask { bool critical; uint32_t lastAliveMs; };

void test_a_critical_task_that_beat_recently_is_alive()
{
    const FakeTask t[] = { {true, 9000}, {true, 8500} };
    TEST_ASSERT_EQUAL_INT(-1, App::Tasks::firstLateCritical(t, 2, 10000, 2000));
}

void test_the_first_late_critical_task_is_named()
{
    const FakeTask t[] = { {true, 9000}, {true, 7000}, {true, 1000} };
    TEST_ASSERT_EQUAL_INT(1, App::Tasks::firstLateCritical(t, 3, 10000, 2000));
}

// Exactly maxAge is still alive; one more millisecond is not.
void test_the_age_limit_is_inclusive()
{
    const FakeTask t[] = { {true, 8000} };
    TEST_ASSERT_EQUAL_INT(-1, App::Tasks::firstLateCritical(t, 1, 10000, 2000));
    TEST_ASSERT_EQUAL_INT(0, App::Tasks::firstLateCritical(t, 1, 10001, 2000));
}

// A stale non-critical task (logbus, ble_svc, cloud) never fails the verdict.
void test_a_non_critical_task_never_counts()
{
    const FakeTask t[] = { {false, 0}, {true, 99000} };
    TEST_ASSERT_EQUAL_INT(-1, App::Tasks::firstLateCritical(t, 2, 100000, 2000));
}

// The task beat after the caller read `now`: that is "just now".
void test_a_beat_after_now_was_read_is_alive()
{
    const FakeTask t[] = { {true, 10005} };
    TEST_ASSERT_EQUAL_INT(-1, App::Tasks::firstLateCritical(t, 1, 10000, 2000));
}

// millis() wraps after 49.7 days; ages across the wrap are still right.
void test_ages_survive_the_millis_wrap()
{
    const FakeTask fresh[] = { {true, 0xFFFFFF00u} };   // 261 ms before now=5
    TEST_ASSERT_EQUAL_INT(-1, App::Tasks::firstLateCritical(fresh, 1, 5, 2000));
    const FakeTask stale[] = { {true, 0xFFFFF000u} };   // 4352 ms before now=0x100
    TEST_ASSERT_EQUAL_INT(0, App::Tasks::firstLateCritical(stale, 1, 0x100, 2000));
}

void test_an_empty_registry_is_alive()
{
    TEST_ASSERT_EQUAL_INT(-1, App::Tasks::firstLateCritical((const FakeTask*)nullptr, 0, 10000, 2000));
}

// ---- OneShot (Step 2) ------------------------------------------------------------
// How a task asks the render owner for something: one slot, latest wins.

static void test_a_one_shot_is_taken_once()
{
    OneShot<uint32_t> o;
    uint32_t v = 0;
    TEST_ASSERT_FALSE(o.take(v));
    o.post(2000);
    TEST_ASSERT_TRUE(o.pending());
    TEST_ASSERT_TRUE(o.take(v));
    TEST_ASSERT_EQUAL(2000u, v);
    TEST_ASSERT_FALSE(o.take(v));
}

static void test_a_newer_request_replaces_one_not_yet_taken()
{
    OneShot<uint8_t> o;
    o.post(1);
    o.post(7);
    uint8_t v = 0;
    TEST_ASSERT_TRUE(o.take(v));
    TEST_ASSERT_EQUAL(7, v);
    TEST_ASSERT_EQUAL(2u, o.posted());
    TEST_ASSERT_EQUAL(1u, o.overwritten());
}

// The web's in-place read: it sees the newest edit, applied or not.
static void test_staged_read_sees_the_newest_edit_in_place()
{
    Staged<int> s;
    s.init(1);
    int seen = 0;
    TEST_ASSERT_TRUE(s.read([&](const int& v) { seen = v; }));
    TEST_ASSERT_EQUAL(1, seen);
    s.edit([](int& v) { v = 5; });
    TEST_ASSERT_TRUE(s.read([&](const int& v) { seen = v; }));
    TEST_ASSERT_EQUAL(5, seen);
    TEST_ASSERT_EQUAL(1, s.live());          // not applied yet
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_snapshot_reads_what_was_published);
    RUN_TEST(test_a_snapshot_counts_publishes_so_a_reader_can_skip_old_news);
    RUN_TEST(test_an_unpublished_snapshot_reads_as_zero);
    RUN_TEST(test_staged_changes_nothing_live_until_the_owner_applies);
    RUN_TEST(test_staged_apply_is_a_buffer_swap_not_a_copy);
    RUN_TEST(test_staged_nothing_to_apply_is_a_cheap_no);
    RUN_TEST(test_staged_owner_never_waits_for_a_writer);
    RUN_TEST(test_staged_edits_accumulate_until_applied);
    RUN_TEST(test_staged_writer_gives_up_rather_than_deadlock);
    RUN_TEST(test_an_empty_histogram_reports_zeros);
    RUN_TEST(test_buckets_are_powers_of_two);
    RUN_TEST(test_percentiles_are_never_optimistic);
    RUN_TEST(test_a_huge_value_lands_in_the_last_bucket);
    RUN_TEST(test_reset_starts_a_new_window);
    RUN_TEST(test_rr_a_request_round_trips);
    RUN_TEST(test_rr_generation_is_never_zero_and_moves_on_every_claim);
    RUN_TEST(test_rr_all_slots_busy_is_a_refusal_not_a_wait);
    RUN_TEST(test_rr_abandoned_before_the_owner_ran_is_skipped);
    RUN_TEST(test_rr_abandoned_while_the_owner_ran_gets_no_answer);
    RUN_TEST(test_rr_a_late_finish_is_still_collected);
    RUN_TEST(test_rr_a_reused_slot_ignores_the_old_generation);
    RUN_TEST(test_rr_a_free_slot_is_preferred_over_an_abandoned_one);
    RUN_TEST(test_rr_a_failed_post_frees_the_slot);
    RUN_TEST(test_rr_an_empty_ticket_matches_nothing);
    RUN_TEST(test_rr_a_stray_wake_does_not_lose_the_slot);
    RUN_TEST(test_rr_a_stray_wake_then_the_answer_is_collected);
    RUN_TEST(test_rr_settle_takes_an_answer_that_beat_the_timeout);
    RUN_TEST(test_a_critical_task_that_beat_recently_is_alive);
    RUN_TEST(test_the_first_late_critical_task_is_named);
    RUN_TEST(test_the_age_limit_is_inclusive);
    RUN_TEST(test_a_non_critical_task_never_counts);
    RUN_TEST(test_a_beat_after_now_was_read_is_alive);
    RUN_TEST(test_ages_survive_the_millis_wrap);
    RUN_TEST(test_an_empty_registry_is_alive);
    RUN_TEST(test_a_one_shot_is_taken_once);
    RUN_TEST(test_a_newer_request_replaces_one_not_yet_taken);
    RUN_TEST(test_staged_read_sees_the_newest_edit_in_place);
    return UNITY_END();
}
