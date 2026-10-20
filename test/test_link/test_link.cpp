#include <unity.h>
#include <string.h>
#include "link/LinkProto.h"

using namespace Link;

void setUp() {}
void tearDown() {}

static Frame frame(uint32_t id, uint8_t len, uint8_t b0)
{
    Frame f;
    f.id = id; f.len = len;
    for (uint8_t i = 0; i < len; ++i) f.data[i] = (uint8_t)(b0 + i);
    return f;
}

static uint8_t feedAll(Decoder& d, const uint8_t* bytes, size_t n, Record& last)
{
    uint8_t got = 0;
    for (size_t i = 0; i < n; ++i) if (d.feed(bytes[i], last)) ++got;
    return got;
}

void test_a_can_frame_survives_the_wire()
{
    Frame in = frame(0x60D, 8, 0x10);
    in.extended = false;
    uint8_t rec[MAX_RECORD];
    const uint8_t n = encodeCan(in, rec);
    TEST_ASSERT_EQUAL_UINT8(4 + 5 + 8, n);

    Decoder d; Record r;
    TEST_ASSERT_EQUAL_UINT8(1, feedAll(d, rec, n, r));
    Frame out;
    TEST_ASSERT_TRUE(r.asCan(out));
    TEST_ASSERT_EQUAL_UINT32(0x60D, out.id);
    TEST_ASSERT_EQUAL_UINT8(8, out.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in.data, out.data, 8);
    TEST_ASSERT_FALSE(out.extended);
    TEST_ASSERT_EQUAL_UINT8(Frame::SRC_VH_CAN, out.source);   // tagged as the car's
}

void test_extended_ids_and_short_frames()
{
    Frame in = frame(0x18DAF110, 3, 0xAA);
    in.extended = true;
    uint8_t rec[MAX_RECORD];
    const uint8_t n = encodeCan(in, rec);
    Decoder d; Record r;
    TEST_ASSERT_EQUAL_UINT8(1, feedAll(d, rec, n, r));
    Frame out;
    TEST_ASSERT_TRUE(r.asCan(out));
    TEST_ASSERT_TRUE(out.extended);
    TEST_ASSERT_EQUAL_UINT32(0x18DAF110, out.id);
    TEST_ASSERT_EQUAL_UINT8(3, out.len);
    TEST_ASSERT_EQUAL_UINT8(0xAC, out.data[2]);
    TEST_ASSERT_EQUAL_UINT8(0, out.data[3]);                  // padded, not stale
}

void test_a_corrupted_byte_costs_one_record_not_the_stream()
{
    uint8_t buf[3 * MAX_RECORD];
    uint8_t n = 0;
    for (uint8_t k = 0; k < 3; ++k) n += encodeCan(frame(0x100 + k, 2, k), buf + n);
    buf[7] ^= 0x40;                                           // inside record 0's payload

    Decoder d; Record r;
    const uint8_t got = feedAll(d, buf, n, r);
    TEST_ASSERT_EQUAL_UINT8(2, got);                          // records 1 and 2
    TEST_ASSERT_EQUAL_UINT32(1, d.crcErrors());
    Frame out; r.asCan(out);
    TEST_ASSERT_EQUAL_UINT32(0x102, out.id);
}

void test_a_lost_byte_resyncs_on_the_next_sync()
{
    uint8_t buf[3 * MAX_RECORD];
    uint8_t n = 0;
    for (uint8_t k = 0; k < 3; ++k) n += encodeCan(frame(0x200 + k, 4, k), buf + n);
    // Drop record 0's length byte: everything after it is misaligned until 0x5A.
    memmove(buf + 2, buf + 3, (size_t)n - 3);
    Decoder d; Record r;
    const uint8_t got = feedAll(d, buf, (size_t)n - 1, r);
    TEST_ASSERT_TRUE(got >= 1);                               // the tail came through
    Frame out; r.asCan(out);
    TEST_ASSERT_EQUAL_UINT32(0x202, out.id);
}

void test_sync_bytes_inside_a_payload_do_not_confuse_the_decoder()
{
    Frame in = frame(0x5A5, 8, SYNC);                         // data full of 0x5A
    for (uint8_t i = 0; i < 8; ++i) in.data[i] = SYNC;
    uint8_t rec[MAX_RECORD];
    const uint8_t n = encodeCan(in, rec);
    Decoder d; Record r;
    TEST_ASSERT_EQUAL_UINT8(1, feedAll(d, rec, n, r));
    Frame out; TEST_ASSERT_TRUE(r.asCan(out));
    TEST_ASSERT_EQUAL_UINT8(SYNC, out.data[7]);
}

void test_status_and_hello_round_trip()
{
    Status s{}; s.frames = 123456; s.forwarded = 777; s.fps = 1500; s.rxErr = 7; s.mode = F_CHANGES; s.busErr = 42; s.state = 1;
    uint8_t rec[MAX_RECORD];
    uint8_t n = encodeStatus(s, rec);
    Decoder d; Record r;
    TEST_ASSERT_EQUAL_UINT8(1, feedAll(d, rec, n, r));
    Status o{}; TEST_ASSERT_TRUE(r.asStatus(o));
    TEST_ASSERT_EQUAL_UINT32(123456, o.frames);
    TEST_ASSERT_EQUAL_UINT16(1500, o.fps);
    TEST_ASSERT_EQUAL_UINT8(F_CHANGES, o.mode);
    TEST_ASSERT_EQUAL_UINT32(42, o.busErr);
    TEST_ASSERT_EQUAL_UINT8(1, o.state);

    n = encodeHello(3, rec);
    TEST_ASSERT_EQUAL_UINT8(1, feedAll(d, rec, n, r));
    TEST_ASSERT_EQUAL_UINT8(T_HELLO, r.type);
    TEST_ASSERT_EQUAL_UINT8(3, r.payload[0]);
}

// THE SIMULATOR'S COMMAND, AND THE FIELD THAT REPORTS IT BACK. The rate has to
// survive the trip in both directions: the brain asks with T_SIM, and board V
// answers in every Status. If the second half broke, the page would go on
// showing "emulator off" while invented frames filled the sniffer's table — the
// one confusion this whole feature has to avoid.
void test_sim_command_and_its_echo_in_status()
{
    uint8_t rec[MAX_RECORD];
    Decoder d; Record r;

    uint8_t n = encodeSim(1500, rec);
    TEST_ASSERT_EQUAL_UINT8(1, feedAll(d, rec, n, r));
    uint16_t fps = 0;
    TEST_ASSERT_TRUE(r.asSim(fps));
    TEST_ASSERT_EQUAL_UINT16(1500, fps);

    n = encodeSim(0, rec);                      // 0 is "stop", not "missing"
    TEST_ASSERT_EQUAL_UINT8(1, feedAll(d, rec, n, r));
    TEST_ASSERT_TRUE(r.asSim(fps));
    TEST_ASSERT_EQUAL_UINT16(0, fps);

    Status s{}; s.sim = 1500; s.state = 1;
    n = encodeStatus(s, rec);
    TEST_ASSERT_EQUAL_UINT8(1, feedAll(d, rec, n, r));
    Status o{}; TEST_ASSERT_TRUE(r.asStatus(o));
    TEST_ASSERT_EQUAL_UINT16(1500, o.sim);

    // A status from the OLD 17-byte firmware must be refused outright rather
    // than decoded with a garbage sim field: mixing versions would otherwise
    // read as "the emulator is on" on a board that has never heard of it.
    Record stale{}; stale.type = T_STATUS; stale.len = 17;
    TEST_ASSERT_FALSE(stale.asStatus(o));
}

void test_filter_round_trip_and_its_limit()
{
    uint32_t ids[9] = {0x181, 0x354, 0x60D, 0x645, 0x0C2, 0x551, 0x5FD, 0x999, 0x998};
    uint8_t rec[MAX_RECORD];
    const uint8_t n = encodeFilter(F_LIST_CHANGES, ids, 9, rec);          // 9 asked, 7 fit
    Decoder d; Record r;
    TEST_ASSERT_EQUAL_UINT8(1, feedAll(d, rec, n, r));
    FilterMode m; uint32_t got[MAX_FILTER_IDS]; uint8_t cnt;
    TEST_ASSERT_TRUE(r.asFilter(m, got, cnt));
    TEST_ASSERT_EQUAL_UINT8(F_LIST_CHANGES, m);
    TEST_ASSERT_EQUAL_UINT8(MAX_FILTER_IDS, cnt);
    TEST_ASSERT_EQUAL_UINT32(0x5FD, got[6]);
}

void test_change_filter_passes_changes_and_the_first_sight_only()
{
    ChangeFilter f; f.setMode(F_CHANGES);
    uint32_t t = 0;
    TEST_ASSERT_TRUE(f.pass(frame(0x60D, 8, 0), t));          // first sight
    for (uint8_t i = 0; i < 50; ++i) TEST_ASSERT_FALSE(f.pass(frame(0x60D, 8, 0), t += 10));
    TEST_ASSERT_TRUE(f.pass(frame(0x60D, 8, 1), t += 10));    // byte changed: the door opened
    TEST_ASSERT_FALSE(f.pass(frame(0x60D, 8, 1), t += 10));
    TEST_ASSERT_TRUE(f.pass(frame(0x60D, 8, 1), t += 1000));  // a second later, regardless
    TEST_ASSERT_TRUE(f.pass(frame(0x181, 2, 9), t));          // another id: its first sight
}

void test_list_modes_drop_everything_off_the_list()
{
    ChangeFilter f;
    const uint32_t want[2] = {0x181, 0x354};
    f.setList(want, 2);
    f.setMode(F_LIST);
    uint32_t t = 0;
    TEST_ASSERT_TRUE(f.pass(frame(0x181, 8, 0), t));
    TEST_ASSERT_TRUE(f.pass(frame(0x181, 8, 0), t += 1));     // every frame, unchanged or not
    TEST_ASSERT_FALSE(f.pass(frame(0x60D, 8, 0), t += 1));
    f.setMode(F_LIST_CHANGES); f.reset();
    TEST_ASSERT_TRUE(f.pass(frame(0x354, 2, 0), t += 1));
    TEST_ASSERT_FALSE(f.pass(frame(0x354, 2, 0), t += 1));
    TEST_ASSERT_FALSE(f.pass(frame(0x60D, 8, 5), t += 1));
}

void test_change_filter_table_overflow_fails_loud_not_silent()
{
    ChangeFilter f; f.setMode(F_CHANGES);
    for (uint16_t id = 0; id < ChangeFilter::MAX_IDS; ++id) f.pass(frame(id, 1, 0), 0);
    TEST_ASSERT_EQUAL_UINT8(ChangeFilter::MAX_IDS, f.tracked());
    // The 97th id has no slot: it is forwarded EVERY time (traffic), never dropped (silence).
    TEST_ASSERT_TRUE(f.pass(frame(0x7FF, 1, 0), 1));
    TEST_ASSERT_TRUE(f.pass(frame(0x7FF, 1, 0), 2));
    TEST_ASSERT_EQUAL_UINT32(2, f.overflow());
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_can_frame_survives_the_wire);
    RUN_TEST(test_extended_ids_and_short_frames);
    RUN_TEST(test_a_corrupted_byte_costs_one_record_not_the_stream);
    RUN_TEST(test_a_lost_byte_resyncs_on_the_next_sync);
    RUN_TEST(test_sync_bytes_inside_a_payload_do_not_confuse_the_decoder);
    RUN_TEST(test_status_and_hello_round_trip);
    RUN_TEST(test_sim_command_and_its_echo_in_status);
    RUN_TEST(test_filter_round_trip_and_its_limit);
    RUN_TEST(test_change_filter_passes_changes_and_the_first_sight_only);
    RUN_TEST(test_list_modes_drop_everything_off_the_list);
    RUN_TEST(test_change_filter_table_overflow_fails_loud_not_silent);
    return UNITY_END();
}
