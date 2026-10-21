#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "utils/LogRing.h"
#include "utils/Log.h"

// The LogBus ring: what GET /api/logs?since=N reads. A reader that falls
// behind must be told how many lines it missed, never handed a line twice or
// a torn one, and the sinks must still get every line the ring accepted.

void setUp() {}
void tearDown() {}

static uint8_t buf[128];

static void pushN(LogRing& r, int from, int to)
{
    char s[16];
    for (int i = from; i <= to; ++i) {
        const int n = snprintf(s, sizeof(s), "line%02d", i);   // 6 bytes + HDR = 12
        TEST_ASSERT_TRUE(r.push(s, (uint16_t)n, (uint32_t)i * 10));
    }
}

static void drain(LogRing& r)
{
    char out[64];
    uint32_t ms;
    while (r.take(out, ms) >= 0) {}
}

void test_numbers_and_read_in_order()
{
    LogRing r(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(0, r.last());
    pushN(r, 1, 3);
    TEST_ASSERT_EQUAL_UINT32(3, r.last());
    LogRing::Cursor c{1, 0, 0};
    char out[64];
    uint32_t ms = 0, lost = 0;
    // Waiting lines are readable before the sinks have them.
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING("line01", out);
    TEST_ASSERT_EQUAL_UINT32(10, ms);
    drain(r);
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING("line02", out);
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING("line03", out);
    TEST_ASSERT_EQUAL_INT(-1, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_UINT32(4, c.n);
    TEST_ASSERT_EQUAL_UINT32(0, lost);
}

void test_since_starts_mid_ring()
{
    LogRing r(buf, sizeof(buf));
    pushN(r, 1, 5);
    drain(r);
    pushN(r, 6, 7);                     // 1..5 kept, 6..7 waiting
    char out[64];
    uint32_t ms, lost = 0;
    LogRing::Cursor c{4, 0, 0};         // since=3
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING("line04", out);
    LogRing::Cursor d{7, 0, 0};         // in the waiting part
    TEST_ASSERT_EQUAL_INT(6, r.read(d, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING("line07", out);
    TEST_ASSERT_EQUAL_UINT32(0, lost);
}

void test_walk_in_steps()
{
    LogRing r(buf, sizeof(buf));
    pushN(r, 1, 8);
    drain(r);
    char out[64];
    uint32_t ms, lost = 0;
    LogRing::Cursor c{8, 0, 0};
    int got, calls = 0;
    while ((got = r.read(c, out, ms, lost, 2)) == -2) ++calls;
    TEST_ASSERT_EQUAL_INT(6, got);
    TEST_ASSERT_EQUAL_STRING("line08", out);
    TEST_ASSERT_TRUE(calls >= 3);
}

void test_wrap_evicts_kept_and_reports_lost()
{
    LogRing r(buf, sizeof(buf));        // 128 / 12 = 10 lines fit
    char out[64];
    uint32_t ms, lost = 0;
    LogRing::Cursor c{1, 0, 0};
    pushN(r, 1, 4);
    drain(r);
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));   // reader at 2
    for (int i = 5; i <= 41; i += 2) { pushN(r, i, i + 1); drain(r); }
    TEST_ASSERT_EQUAL_UINT32(42, r.last());
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));
    // 42 - 10 + 1 = 33 is the oldest held: 2..32 were lost.
    TEST_ASSERT_EQUAL_STRING("line33", out);
    TEST_ASSERT_EQUAL_UINT32(31, lost);
    int n = 1;
    while (r.read(c, out, ms, lost, 100) >= 0) ++n;
    TEST_ASSERT_EQUAL_INT(10, n);
    TEST_ASSERT_EQUAL_STRING("line42", out);
}

void test_cursor_evicted_between_reads()
{
    LogRing r(buf, sizeof(buf));
    char out[64];
    uint32_t ms, lost = 0;
    pushN(r, 1, 10);
    drain(r);
    LogRing::Cursor c{1, 0, 0};
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));   // has 1, wants 2
    pushN(r, 11, 13);                   // evicts 1..3
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING("line04", out);
    TEST_ASSERT_EQUAL_UINT32(2, lost);
}

void test_waiting_lines_are_never_evicted()
{
    LogRing r(buf, sizeof(buf));
    pushN(r, 1, 10);                    // full, nothing taken
    TEST_ASSERT_FALSE(r.push("0123456789", 10, 0));
    TEST_ASSERT_EQUAL_UINT32(10, r.last());
    char out[64];
    uint32_t ms;
    int n = 0;
    while (r.take(out, ms) >= 0) ++n;
    TEST_ASSERT_EQUAL_INT(10, n);
    TEST_ASSERT_EQUAL_STRING("line10", out);
    TEST_ASSERT_TRUE(r.push("0123456789", 10, 0));   // now room, by evicting kept lines
}

void test_line_wraps_across_the_end()
{
    LogRing r(buf, sizeof(buf));
    char out[64];
    uint32_t ms, lost = 0;
    pushN(r, 1, 8);                     // 96 bytes: the next 43 wrap
    drain(r);
    const char* big = "a long line that wraps around the end";   // 37 + 6 = 43
    TEST_ASSERT_TRUE(r.push(big, (uint16_t)strlen(big), 7));
    TEST_ASSERT_EQUAL_INT((int)strlen(big), r.take(out, ms));
    TEST_ASSERT_EQUAL_STRING(big, out);
    LogRing::Cursor c{9, 0, 0};
    TEST_ASSERT_EQUAL_INT((int)strlen(big), r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING(big, out);
    TEST_ASSERT_EQUAL_UINT32(7, ms);
    TEST_ASSERT_EQUAL_UINT32(0, lost);
}

void test_empty_line()
{
    LogRing r(buf, sizeof(buf));
    char out[64];
    uint32_t ms, lost = 0;
    TEST_ASSERT_TRUE(r.push("", 0, 5));
    pushN(r, 2, 2);
    LogRing::Cursor c{1, 0, 0};
    TEST_ASSERT_EQUAL_INT(0, r.read(c, out, ms, lost, 100));   // a line, not "up to date"
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_UINT32(5, ms);
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING("line02", out);
    // The sinks get the empty line too, and the drain does not stop at it.
    TEST_ASSERT_EQUAL_INT(0, r.take(out, ms));
    TEST_ASSERT_EQUAL_INT(6, r.take(out, ms));
    TEST_ASSERT_EQUAL_INT(-1, r.take(out, ms));
}

void test_take_interleaved_with_a_reader_in_the_waiting_part()
{
    LogRing r(buf, sizeof(buf));
    char out[64];
    uint32_t ms, lost = 0;
    pushN(r, 1, 6);                     // all waiting
    LogRing::Cursor c{3, 0, 0};
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING("line03", out);
    // The sinks take past the reader's place: it stays valid (kept now).
    for (int i = 0; i < 5; ++i) TEST_ASSERT_EQUAL_INT(6, r.take(out, ms));
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING("line04", out);
    pushN(r, 7, 8);
    TEST_ASSERT_EQUAL_INT(6, r.take(out, ms));                 // line06
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING("line05", out);
    int n = 0;
    while (r.read(c, out, ms, lost, 100) >= 0) ++n;
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("line08", out);
    TEST_ASSERT_EQUAL_UINT32(0, lost);
}

void test_drops_keep_numbers_continuous()
{
    LogRing r(buf, sizeof(buf));
    char out[64];
    uint32_t ms, lost = 0, dropped = 0;
    pushN(r, 1, 10);                    // full of waiting lines
    for (int i = 0; i < 3; ++i) if (!r.push("0123456789", 10, 0)) ++dropped;
    TEST_ASSERT_EQUAL_UINT32(3, dropped);
    TEST_ASSERT_EQUAL_UINT32(10, r.last());   // a refused line takes no number
    TEST_ASSERT_EQUAL_INT(6, r.take(out, ms));
    TEST_ASSERT_EQUAL_INT(6, r.take(out, ms));
    TEST_ASSERT_TRUE(r.push("next", 4, 0));
    TEST_ASSERT_EQUAL_UINT32(11, r.last());
    LogRing::Cursor c{10, 0, 0};
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING("line10", out);
    TEST_ASSERT_EQUAL_INT(4, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING("next", out);
    TEST_ASSERT_EQUAL_INT(-1, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_UINT32(0, lost);
}

// LogBus::grow(): the boot ring's lines move into the big one. A wrapped ring
// with kept and waiting lines must come out with the same numbers, the same
// split, and room to go on.
void test_move_keeps_numbers_and_the_split()
{
    LogRing r(buf, sizeof(buf));
    char out[64];
    uint32_t ms, lost = 0;
    for (int i = 1; i <= 13; i += 2) { pushN(r, i, i + 1); drain(r); }   // wrapped
    pushN(r, 15, 16);                   // 7..14 kept, 15..16 waiting
    static uint8_t big[512];
    TEST_ASSERT_TRUE(r.moveTo(big, sizeof(big)));
    TEST_ASSERT_EQUAL_UINT32(16, r.last());
    // The kept lines are still there for a reader, from the oldest held.
    LogRing::Cursor c{1, 0, 0};
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));
    TEST_ASSERT_EQUAL_STRING("line07", out);
    TEST_ASSERT_EQUAL_UINT32(70, ms);
    TEST_ASSERT_EQUAL_UINT32(6, lost);
    // The waiting lines still go to the sinks, once.
    TEST_ASSERT_EQUAL_INT(6, r.take(out, ms));
    TEST_ASSERT_EQUAL_STRING("line15", out);
    TEST_ASSERT_EQUAL_INT(6, r.take(out, ms));
    TEST_ASSERT_EQUAL_STRING("line16", out);
    TEST_ASSERT_EQUAL_INT(-1, r.take(out, ms));
    // The new size is used: 40 more lines (480 bytes) fit without losing 7.
    pushN(r, 17, 40);
    LogRing::Cursor d{7, 0, 0};
    lost = 0;
    int n = 0;
    while (r.read(d, out, ms, lost, 100) >= 0) ++n;
    TEST_ASSERT_EQUAL_INT(34, n);
    TEST_ASSERT_EQUAL_STRING("line40", out);
    TEST_ASSERT_EQUAL_UINT32(0, lost);
}

void test_move_refuses_a_buffer_too_small()
{
    LogRing r(buf, sizeof(buf));
    char out[64];
    uint32_t ms, lost = 0;
    pushN(r, 1, 5);                     // 60 bytes held
    static uint8_t small[48];
    TEST_ASSERT_FALSE(r.moveTo(small, sizeof(small)));
    LogRing::Cursor c{1, 0, 0};
    TEST_ASSERT_EQUAL_INT(6, r.read(c, out, ms, lost, 100));   // untouched
    TEST_ASSERT_EQUAL_STRING("line01", out);
    TEST_ASSERT_EQUAL_INT(6, r.take(out, ms));
}

static const char* tagOf(const char* s, uint8_t* level = nullptr, uint16_t* msg = nullptr)
{
    const LogLine l = LogLine::parse(s, (uint32_t)strlen(s));
    if (level) *level = l.level;
    if (msg) *msg = l.msg;
    return Log::TAG_ROWS[l.tag].tag;
}

void test_parse_level_and_tag()
{
    uint8_t lv;
    uint16_t m;
    TEST_ASSERT_EQUAL_STRING("BLE", tagOf("W [BLE] bonded", &lv, &m));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)LogLevel::WRN, lv);
    TEST_ASSERT_EQUAL_UINT16(2, m);
    TEST_ASSERT_EQUAL_STRING("BLE", tagOf("I [BT] x"));
    TEST_ASSERT_EQUAL_STRING("BLE", tagOf("E [AMS] x"));
    TEST_ASSERT_EQUAL_STRING("NET", tagOf("I [web] log level -> DBG"));
    TEST_ASSERT_EQUAL_STRING("KEY", tagOf("[kev] VOL_UP down", &lv, &m));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)LogLevel::INF, lv);
    TEST_ASSERT_EQUAL_UINT16(0, m);
    TEST_ASSERT_EQUAL_STRING("KEY", tagOf("[panelkey] x"));
    TEST_ASSERT_EQUAL_STRING("HU", tagOf("[hu] T 7D 03"));
    TEST_ASSERT_EQUAL_STRING("HU", tagOf("[hur] R cmd=01"));
    TEST_ASSERT_EQUAL_STRING("CAN", tagOf("[can2] up"));
    TEST_ASSERT_EQUAL_STRING("CAP", tagOf("[cap] start"));
    TEST_ASSERT_EQUAL_STRING("MAC", tagOf("[macro] fired"));
    TEST_ASSERT_EQUAL_STRING("PNL", tagOf("[disp] 1C1 open"));
    TEST_ASSERT_EQUAL_STRING("SYS", tagOf("[hb] up=00:01"));
    TEST_ASSERT_EQUAL_STRING("SYS", tagOf("can role = display"));
    TEST_ASSERT_EQUAL_STRING("SYS", tagOf("[can"));          // no closing bracket
    TEST_ASSERT_EQUAL_STRING("SYS", tagOf("[cann] x"));      // a longer word
    TEST_ASSERT_EQUAL_STRING("SYS", tagOf("[ca] x"));        // a shorter one
    TEST_ASSERT_EQUAL_STRING("SYS", tagOf("I ["));
    TEST_ASSERT_EQUAL_STRING("SYS", tagOf(""));
    TEST_ASSERT_EQUAL_STRING("SYS", Log::TAG_ROWS[Log::TAG_COUNT - 1].tag);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_numbers_and_read_in_order);
    RUN_TEST(test_since_starts_mid_ring);
    RUN_TEST(test_walk_in_steps);
    RUN_TEST(test_wrap_evicts_kept_and_reports_lost);
    RUN_TEST(test_cursor_evicted_between_reads);
    RUN_TEST(test_waiting_lines_are_never_evicted);
    RUN_TEST(test_line_wraps_across_the_end);
    RUN_TEST(test_parse_level_and_tag);
    RUN_TEST(test_empty_line);
    RUN_TEST(test_take_interleaved_with_a_reader_in_the_waiting_part);
    RUN_TEST(test_drops_keep_numbers_continuous);
    RUN_TEST(test_move_keeps_numbers_and_the_split);
    RUN_TEST(test_move_refuses_a_buffer_too_small);
    return UNITY_END();
}
