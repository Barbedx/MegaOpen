// Who may write the head-unit UART, and when (docs/design/event-tasks.md §3.5).
//
// The promise is a number: an ack or a key never waits behind more than one
// vehicle frame, which is L_max * 260 us. The head unit drops an ack later than
// 10 ms, and a dropped ack makes it resend, so telemetry that sits in front of
// an ack is not slow, it is wrong. These tests hold the gate to that promise,
// and measure L_max over the real Rav4 table so the number cannot drift when a
// message is added.
#include <unity.h>
#include <string.h>

#include "sinks/UartLanes.h"
#include "proto/canbox/Rav4Emitter.h"
#include "proto/canbox/RaiseFramer.h"
#include "vehicle/VehicleState.h"

using namespace Sinks::UartLanes;

void setUp() {}
void tearDown() {}

// ---- priority --------------------------------------------------------------

static uint8_t bit(Lane l) { return (uint8_t)(1u << (uint8_t)l); }

static void test_ack_beats_key_beats_reply_beats_vehicle()
{
    TEST_ASSERT_EQUAL((int)Lane::Ack,     pick(bit(Lane::Ack) | bit(Lane::Key) | bit(Lane::Reply) | bit(Lane::Vehicle)));
    TEST_ASSERT_EQUAL((int)Lane::Key,     pick(bit(Lane::Key) | bit(Lane::Reply) | bit(Lane::Vehicle)));
    TEST_ASSERT_EQUAL((int)Lane::Reply,   pick(bit(Lane::Reply) | bit(Lane::Vehicle)));
    TEST_ASSERT_EQUAL((int)Lane::Vehicle, pick(bit(Lane::Vehicle)));
    TEST_ASSERT_EQUAL(-1, pick(0));
}

// ---- the gate --------------------------------------------------------------

static void test_acks_and_keys_are_always_written()
{
    TEST_ASSERT_TRUE(mayWrite(Lane::Ack, FIFO_BYTES, true, 1, 0));
    TEST_ASSERT_TRUE(mayWrite(Lane::Key, 100, true, 6, 0));
}

static void test_a_vehicle_frame_needs_an_empty_fifo()
{
    TEST_ASSERT_TRUE (mayWrite(Lane::Vehicle, 0, false, 16, 1000));
    TEST_ASSERT_FALSE(mayWrite(Lane::Vehicle, 1, false, 16, 1000));
    TEST_ASSERT_FALSE(mayWrite(Lane::Reply,   1, false, 16, 1000));
}

static void test_a_vehicle_frame_waits_while_the_head_unit_is_mid_frame()
{
    TEST_ASSERT_FALSE(mayWrite(Lane::Vehicle, 0, true, 6, 1000));
    TEST_ASSERT_FALSE(mayWrite(Lane::Reply,   0, true, 6, 1000));
}

static void test_a_long_frame_also_waits_for_a_quiet_link()
{
    TEST_ASSERT_FALSE(mayWrite(Lane::Vehicle, 0, false, QUIET_LEN_MAX + 1, QUIET_MS - 1));
    TEST_ASSERT_TRUE (mayWrite(Lane::Vehicle, 0, false, QUIET_LEN_MAX + 1, QUIET_MS));
    TEST_ASSERT_TRUE (mayWrite(Lane::Vehicle, 0, false, QUIET_LEN_MAX, 0));
}

// A burst of keys (a macro replaying held taps) may not stack more in front of
// the next ack than one vehicle frame would; a single key always goes.
static void test_a_key_burst_stops_at_the_key_budget()
{
    const uint8_t KEY = 6;
    TEST_ASSERT_TRUE (keyFits(0, KEY));
    TEST_ASSERT_TRUE (keyFits(0, 40));                  // an empty FIFO takes anything
    TEST_ASSERT_TRUE (keyFits(KEY_BUDGET - KEY, KEY));
    TEST_ASSERT_FALSE(keyFits(KEY_BUDGET - KEY + 1, KEY));
    TEST_ASSERT_FALSE(keyFits(QUIET_LEN_MAX, KEY));     // behind a full vehicle frame: next pass

    // Twelve frames written greedily: the FIFO never holds more than the budget.
    uint16_t inflight = 0;
    uint8_t written = 0;
    for (uint8_t i = 0; i < 12 && keyFits(inflight, KEY); ++i) { inflight += KEY; ++written; }
    TEST_ASSERT_EQUAL(KEY_BUDGET / KEY, written);
    TEST_ASSERT_TRUE(wireUs(inflight) <= wireUs(QUIET_LEN_MAX));
}

static void test_the_parser_says_when_it_is_inside_a_frame()
{
    Canbox::RxParser p;
    const uint8_t f[] = { 0x2E, 0x9D, 0x03, 0x0E, 0x05, 0x00 };
    TEST_ASSERT_FALSE(p.midFrame());
    p.feed(f[0]);
    TEST_ASSERT_TRUE(p.midFrame());
    for (uint8_t i = 1; i < sizeof(f); ++i) p.feed(f[i]);
    TEST_ASSERT_TRUE(p.midFrame());                     // the checksum is still owed
    p.feed(Canbox::checksum(0x9D, 0x03, f + 3));
    TEST_ASSERT_FALSE(p.midFrame());
}

static void test_inflight_is_what_the_fifo_still_holds()
{
    TEST_ASSERT_EQUAL(0,   inflight(FIFO_BYTES));
    TEST_ASSERT_EQUAL(28,  inflight(100));
    TEST_ASSERT_EQUAL(FIFO_BYTES, inflight(0));
    TEST_ASSERT_EQUAL(FIFO_BYTES, inflight(-1));
}

// ---- L_max over the real table ----------------------------------------------

// Every message built with every signal known, in each of the forms the tuning
// can select: the longest frame the emitter can ever produce.
static uint8_t lmax()
{
    Vehicle::State s;
    for (uint8_t i = 0; i < Vehicle::SIG_COUNT; ++i) s.pin(i, 1);
    uint8_t best = 0;
    for (uint8_t form = 0; form < 4; ++form) {
        Canbox::Rav4Emitter em;
        Canbox::Tuning t;
        t.cmd28    = form & 1;
        t.doorsLen = (form & 2) ? 2 : 1;
        t.histLen  = 13;
        em.setTuning(t);
        for (uint8_t m = 0; m < Canbox::Rav4Emitter::messageCount(); ++m) {
            uint8_t frame[Canbox::MAX_FRAME];
            uint8_t len = 0;
            if (em.preview(m, s, frame, len) && len > best) best = len;
        }
    }
    return best;
}

static void test_every_rav4_message_fits_under_the_quiet_limit()
{
    const uint8_t L = lmax();
    TEST_ASSERT_TRUE_MESSAGE(L > 0, "no message built: the table did not run");
    // If this fails, a message grew past 24 B: either shorten it or accept
    // that it waits for a quiet link (QUIET_MS), and re-check the ack budget.
    TEST_ASSERT_LESS_OR_EQUAL(QUIET_LEN_MAX, L);
    // 6.2 ms is the share of the 10 ms ack window the plan allows telemetry.
    TEST_ASSERT_LESS_OR_EQUAL(6240u, wireUs(L));
}

// ---- the promise, simulated --------------------------------------------------

// A FIFO that drains one byte per 260 us. The emitter offers a frame whenever
// it likes (as fast as every 1 ms); an ack turns up at an arbitrary moment.
// The ack's wait is whatever is in the FIFO in front of it.
static void test_an_ack_never_waits_behind_more_than_one_vehicle_frame()
{
    const uint8_t L = lmax();
    uint32_t worstUs = 0;
    // Deterministic pseudo-random sweep over offer and ack timings.
    uint32_t seed = 12345;
    auto rnd = [&seed]() { seed = seed * 1103515245u + 12345u; return seed >> 16; };

    for (int trial = 0; trial < 2000; ++trial) {
        uint32_t fifo = 0;              // bytes in the FIFO
        uint32_t carryUs = 0;           // partial byte time
        const uint32_t ackAtUs = (rnd() % 50000) + 5000;
        for (uint32_t tUs = 0; tUs <= ackAtUs; tUs += 100) {
            // Drain.
            carryUs += 100;
            while (carryUs >= US_PER_BYTE && fifo) { --fifo; carryUs -= US_PER_BYTE; }
            if (!fifo) carryUs = 0;
            // The emitter tries a frame of a random length up to L every ms.
            if (tUs % 1000 == 0) {
                const uint8_t len = (uint8_t)(6 + rnd() % (L - 5));
                if (mayWrite(Lane::Vehicle, (uint16_t)fifo, false, len, 1000)) fifo += len;
            }
        }
        const uint32_t waitUs = wireUs(fifo);
        if (waitUs > worstUs) worstUs = waitUs;
    }
    TEST_ASSERT_LESS_OR_EQUAL(wireUs(L), worstUs);
}

static void test_the_old_reserve_gate_would_have_broken_the_promise()
{
    // The gate this replaced wrote a vehicle frame while 24 bytes stayed free:
    // up to 104 bytes, 27 ms, in front of an ack. Kept as a number so the
    // reason for the empty-FIFO rule is in the tests, not only in a comment.
    TEST_ASSERT_GREATER_THAN(10000u, wireUs(FIFO_BYTES - 24));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_ack_beats_key_beats_reply_beats_vehicle);
    RUN_TEST(test_acks_and_keys_are_always_written);
    RUN_TEST(test_a_vehicle_frame_needs_an_empty_fifo);
    RUN_TEST(test_a_vehicle_frame_waits_while_the_head_unit_is_mid_frame);
    RUN_TEST(test_a_long_frame_also_waits_for_a_quiet_link);
    RUN_TEST(test_inflight_is_what_the_fifo_still_holds);
    RUN_TEST(test_a_key_burst_stops_at_the_key_budget);
    RUN_TEST(test_the_parser_says_when_it_is_inside_a_frame);

    RUN_TEST(test_every_rav4_message_fits_under_the_quiet_limit);
    RUN_TEST(test_an_ack_never_waits_behind_more_than_one_vehicle_frame);
    RUN_TEST(test_the_old_reserve_gate_would_have_broken_the_promise);
    return UNITY_END();
}
