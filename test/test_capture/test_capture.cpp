// The capture format and the change-only filter (docs/design/event-tasks.md,
// Step 5b). A capture is read on the PC weeks after it was recorded in the car:
// a byte in the wrong place is a drive that cannot be decoded, and a filter
// that skips one change too many is a signal that "never moves". Both are
// invisible on the board, so they are held here.
#include <unity.h>
#include <string.h>
#include <initializer_list>
#include "capture/CapFormat.h"

using namespace CapFormat;

void setUp() {}
void tearDown() {}

static Rec frame(uint32_t t, uint32_t id, uint8_t bus, std::initializer_list<uint8_t> bytes, bool ext = false)
{
    Rec r;
    r.t = t;
    r.w = idWord(id, ext, bus);
    r.kind = FRAME;
    r.len = (uint8_t)bytes.size();
    uint8_t i = 0;
    for (uint8_t b : bytes) r.data[i++] = b;
    return r;
}

// ---- layout --------------------------------------------------------------------

static void test_the_sizes_are_the_documented_ones()
{
    TEST_ASSERT_EQUAL(64, HEADER_SIZE);
    TEST_ASSERT_EQUAL(18, REC_SIZE);
}

static void test_the_id_word_packs_id_bus_and_ext()
{
    const uint32_t w = idWord(0x18DAF110, true, 1);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF110, idOf(w));
    TEST_ASSERT_EQUAL(1, busOf(w));
    TEST_ASSERT_TRUE(extOf(w));
    const uint32_t s = idWord(0x7FF, false, 0);
    TEST_ASSERT_EQUAL_HEX32(0x7FF, s);
    TEST_ASSERT_FALSE(extOf(s));
}

static void test_a_header_round_trips()
{
    Header h;
    h.session = 12; h.part = 3; h.mode = MODE_FULL;
    h.bitrate[0] = 500000; h.bitrate[1] = 250000;
    h.boot = 77; h.wallSec = 1760000000; h.wallAtMs = 4321;
    memcpy(h.build, "abc1234-dirty@2026-10-15", 24);   // all 24 used, no NUL
    uint8_t b[HEADER_SIZE];
    encodeHeader(h, b);
    TEST_ASSERT_EQUAL_MEMORY("MOCAP1", b, 6);
    TEST_ASSERT_EQUAL(64, b[6]);
    TEST_ASSERT_EQUAL(0, b[63]);   // the build id is cut, and NUL-ended

    Header g;
    TEST_ASSERT_TRUE(decodeHeader(b, sizeof(b), g));
    TEST_ASSERT_EQUAL(12, g.session);
    TEST_ASSERT_EQUAL(3, g.part);
    TEST_ASSERT_EQUAL(MODE_FULL, g.mode);
    TEST_ASSERT_EQUAL(500000, g.bitrate[0]);
    TEST_ASSERT_EQUAL(250000, g.bitrate[1]);
    TEST_ASSERT_EQUAL(77, g.boot);
    TEST_ASSERT_EQUAL(1760000000u, g.wallSec);
    TEST_ASSERT_EQUAL(4321, g.wallAtMs);
    TEST_ASSERT_EQUAL(KEYFRAME_MS, g.keyframeMs);
    TEST_ASSERT_EQUAL(STATS_MS, g.statsMs);
    TEST_ASSERT_EQUAL_STRING_LEN("abc1234-dirty@2026-10-15", g.build, 23);
}

static void test_a_wrong_magic_or_a_short_header_is_refused()
{
    Header h, g;
    uint8_t b[HEADER_SIZE];
    encodeHeader(h, b);
    TEST_ASSERT_FALSE(decodeHeader(b, HEADER_SIZE - 1, g));
    b[0] = 'X';
    TEST_ASSERT_FALSE(decodeHeader(b, HEADER_SIZE, g));
}

static void test_a_record_round_trips_little_endian()
{
    Rec r = frame(0x01020304, 0x123, 1, {0xDE, 0xAD, 0xBE});
    uint8_t b[REC_SIZE];
    encodeRec(r, b);
    TEST_ASSERT_EQUAL_HEX8(0x04, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, b[3]);
    TEST_ASSERT_EQUAL_HEX8(0x23, b[4]);
    TEST_ASSERT_EQUAL_HEX8(0x01, b[5]);    // 0x123 | 1<<29 = 0x20000123
    TEST_ASSERT_EQUAL_HEX8(0x20, b[7]);
    TEST_ASSERT_EQUAL(3, b[8]);
    TEST_ASSERT_EQUAL(FRAME, b[9]);
    TEST_ASSERT_EQUAL_HEX8(0xDE, b[10]);
    TEST_ASSERT_EQUAL_HEX8(0x00, b[17]);
    Rec g;
    decodeRec(b, g);
    TEST_ASSERT_EQUAL(r.t, g.t);
    TEST_ASSERT_EQUAL(r.w, g.w);
    TEST_ASSERT_EQUAL(3, g.len);
    TEST_ASSERT_EQUAL_MEMORY(r.data, g.data, 8);
}

// ---- markers -------------------------------------------------------------------

static void test_a_marker_spans_pieces_and_reassembles()
{
    Rec m[32];
    const char* text = "fuel 3/4, range 410";   // 19 bytes: 3 pieces
    TEST_ASSERT_EQUAL(3, markerPieces(text));
    TEST_ASSERT_EQUAL(3, markerRecs(7, text, 1000, m, 32));
    TEST_ASSERT_EQUAL(MARKER, m[0].kind);
    TEST_ASSERT_EQUAL(8, m[0].len);
    TEST_ASSERT_EQUAL(3, m[2].len);
    TEST_ASSERT_EQUAL(2, (m[2].w >> 24) & 0xFF);
    TEST_ASSERT_EQUAL(19, (m[1].w >> 16) & 0xFF);
    char out[128];
    TEST_ASSERT_EQUAL(19, markerText(m, 3, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(text, out);
}

static void test_an_empty_marker_is_one_piece()
{
    Rec m[2];
    TEST_ASSERT_EQUAL(1, markerRecs(1, "", 5, m, 2));
    char out[8];
    TEST_ASSERT_EQUAL(0, markerText(m, 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_a_marker_never_goes_out_by_half()
{
    Rec m[2];
    TEST_ASSERT_EQUAL(0, markerRecs(1, "seventeen chars!!", 5, m, 2));
}

static void test_a_long_marker_is_cut_at_the_limit()
{
    char text[200];
    memset(text, 'a', sizeof(text) - 1);
    text[199] = 0;
    Rec m[32];
    const size_t n = markerRecs(2, text, 5, m, 32);
    TEST_ASSERT_EQUAL(MARK_TEXT_MAX / 8, n);
    char out[256];
    TEST_ASSERT_EQUAL((int)MARK_TEXT_MAX, markerText(m, n, out, sizeof(out)));
}

static void test_mixed_up_pieces_are_not_a_marker()
{
    Rec a[4], b[4];
    markerRecs(1, "0123456789", 5, a, 4);
    markerRecs(2, "0123456789", 5, b, 4);
    Rec mix[2] = {a[0], b[1]};
    char out[32];
    TEST_ASSERT_EQUAL(-1, markerText(mix, 2, out, sizeof(out)));
    TEST_ASSERT_EQUAL(-1, markerText(a, 1, out, sizeof(out)));   // one piece missing
}

// ---- the change-only filter -------------------------------------------------------

static ChangeFilter::Entry s_table[64];

static void test_first_seen_is_written_and_a_repeat_is_not()
{
    ChangeFilter f;
    f.begin(s_table, 64);
    TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(10, 0x100, 0, {1, 2})));
    TEST_ASSERT_EQUAL(ChangeFilter::SKIP, f.onFrame(frame(20, 0x100, 0, {1, 2})));
    TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(30, 0x100, 0, {1, 3})));
    TEST_ASSERT_EQUAL(ChangeFilter::SKIP, f.onFrame(frame(40, 0x100, 0, {1, 3})));
    TEST_ASSERT_EQUAL(1, f.ids());
}

static void test_an_all_zero_frame_is_still_first_seen()
{
    ChangeFilter f;
    f.begin(s_table, 64);
    TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(0, 0x0, 0, {})));
    TEST_ASSERT_EQUAL(ChangeFilter::SKIP, f.onFrame(frame(1, 0x0, 0, {})));
}

static void test_a_length_change_is_a_change()
{
    ChangeFilter f;
    f.begin(s_table, 64);
    f.onFrame(frame(0, 0x200, 0, {1, 2}));
    TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(1, 0x200, 0, {1, 2, 0})));
}

static void test_same_id_on_two_buses_or_ext_is_two_ids()
{
    ChangeFilter f;
    f.begin(s_table, 64);
    TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(0, 0x123, 0, {1})));
    TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(0, 0x123, 1, {1})));
    TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(0, 0x123, 0, {1}, true)));
    TEST_ASSERT_EQUAL(3, f.ids());
}

static void test_an_unchanged_id_gets_a_keyframe_every_5_s()
{
    ChangeFilter f;
    f.begin(s_table, 64);
    TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(1000, 0x300, 0, {9})));
    TEST_ASSERT_EQUAL(ChangeFilter::SKIP, f.onFrame(frame(5999, 0x300, 0, {9})));
    TEST_ASSERT_EQUAL(KEYFRAME, f.onFrame(frame(6000, 0x300, 0, {9})));
    TEST_ASSERT_EQUAL(ChangeFilter::SKIP, f.onFrame(frame(6100, 0x300, 0, {9})));
    // A change restarts the 5 s.
    TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(9000, 0x300, 0, {8})));
    TEST_ASSERT_EQUAL(ChangeFilter::SKIP, f.onFrame(frame(13999, 0x300, 0, {8})));
    TEST_ASSERT_EQUAL(KEYFRAME, f.onFrame(frame(14000, 0x300, 0, {8})));
}

static void test_keyframes_survive_the_millis_wrap()
{
    ChangeFilter f;
    f.begin(s_table, 64);
    f.onFrame(frame(0xFFFFF000u, 0x301, 0, {1}));
    TEST_ASSERT_EQUAL(ChangeFilter::SKIP, f.onFrame(frame(0x00000100u, 0x301, 0, {1})));
    TEST_ASSERT_EQUAL(KEYFRAME, f.onFrame(frame(0xFFFFF000u + 5000u, 0x301, 0, {1})));
}

struct StatSink { Rec r[64]; size_t n = 0; };
static void collect(const Rec& r, void* ctx)
{
    StatSink* s = (StatSink*)ctx;
    if (s->n < 64) s->r[s->n++] = r;
}

static uint32_t u32At(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void test_stats_recover_the_rate_of_skipped_repeats()
{
    ChangeFilter f;
    f.begin(s_table, 64);
    f.startStats(0);
    for (uint32_t t = 0; t < 1000; t += 10) f.onFrame(frame(t, 0x400, 0, {(uint8_t)(t < 500 ? 1 : 2)}));
    for (uint32_t t = 0; t < 1000; t += 100) f.onFrame(frame(t, 0x401, 0, {7}));
    TEST_ASSERT_FALSE(f.statsDue(999));
    TEST_ASSERT_TRUE(f.statsDue(1000));
    StatSink s;
    TEST_ASSERT_EQUAL(2, f.emitStats(1000, collect, &s));
    for (size_t i = 0; i < s.n; ++i) {
        TEST_ASSERT_EQUAL(STATS, s.r[i].kind);
        const uint32_t count = u32At(s.r[i].data);
        const uint16_t ms = (uint16_t)(s.r[i].data[4] | (s.r[i].data[5] << 8));
        const uint16_t ch = (uint16_t)(s.r[i].data[6] | (s.r[i].data[7] << 8));
        TEST_ASSERT_EQUAL(1000, ms);
        if (idOf(s.r[i].w) == 0x400) { TEST_ASSERT_EQUAL(100, count); TEST_ASSERT_EQUAL(2, ch); }
        else                          { TEST_ASSERT_EQUAL(10, count);  TEST_ASSERT_EQUAL(1, ch); }
    }
    // Counts reset: a silent second emits nothing.
    StatSink t;
    TEST_ASSERT_EQUAL(0, f.emitStats(2000, collect, &t));
}

static void test_a_full_table_writes_every_frame_and_counts_it()
{
    ChangeFilter f;
    ChangeFilter::Entry small[8];
    f.begin(small, 8);
    // 7 of 8 slots are usable.
    for (uint32_t id = 0; id < 7; ++id) TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(0, id, 0, {1})));
    TEST_ASSERT_EQUAL(7, f.ids());
    TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(0, 0x700, 0, {1})));
    TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(1, 0x700, 0, {1})));
    TEST_ASSERT_EQUAL(2, f.overflow());
    // The ids already in it keep their change-only behaviour.
    TEST_ASSERT_EQUAL(ChangeFilter::SKIP, f.onFrame(frame(2, 3, 0, {1})));
}

static void test_reset_forgets_every_id()
{
    ChangeFilter f;
    f.begin(s_table, 64);
    f.onFrame(frame(0, 0x500, 0, {1}));
    f.reset();
    TEST_ASSERT_EQUAL(0, f.ids());
    TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(1, 0x500, 0, {1})));
}

static void test_rekey_writes_each_id_again_and_keeps_its_counts()
{
    ChangeFilter f;
    f.begin(s_table, 64);
    f.startStats(0);
    f.onFrame(frame(0, 0x510, 0, {1}));
    f.onFrame(frame(10, 0x510, 0, {1}));
    f.rekey();
    TEST_ASSERT_EQUAL(1, f.ids());
    TEST_ASSERT_EQUAL(FRAME, f.onFrame(frame(20, 0x510, 0, {1})));
    TEST_ASSERT_EQUAL(ChangeFilter::SKIP, f.onFrame(frame(30, 0x510, 0, {1})));
    StatSink s;
    TEST_ASSERT_EQUAL(1, f.emitStats(1000, collect, &s));
    TEST_ASSERT_EQUAL(4, u32At(s.r[0].data));
}

// A stream through filter + encoder, decoded back: what the PC sees reproduces
// every change, in order.
static void test_a_change_only_stream_reproduces_every_change()
{
    ChangeFilter f;
    f.begin(s_table, 64);
    uint8_t file[4096];
    size_t n = 0;
    uint8_t lastVal = 0xFF;
    int changes = 0;
    for (uint32_t t = 0; t < 3000; t += 10) {
        const uint8_t v = (uint8_t)(t / 700);   // changes 4 times
        if (v != lastVal) { ++changes; lastVal = v; }
        Rec r = frame(t, 0x600, 0, {0xAA, v});
        if (f.onFrame(r) != ChangeFilter::SKIP) { encodeRec(r, file + n); n += REC_SIZE; }
    }
    TEST_ASSERT_EQUAL(changes, (int)(n / REC_SIZE));
    uint8_t prev = 0xFF;
    for (size_t off = 0; off < n; off += REC_SIZE) {
        Rec g;
        decodeRec(file + off, g);
        TEST_ASSERT_NOT_EQUAL(prev, g.data[1]);
        prev = g.data[1];
    }
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_sizes_are_the_documented_ones);
    RUN_TEST(test_the_id_word_packs_id_bus_and_ext);
    RUN_TEST(test_a_header_round_trips);
    RUN_TEST(test_a_wrong_magic_or_a_short_header_is_refused);
    RUN_TEST(test_a_record_round_trips_little_endian);
    RUN_TEST(test_a_marker_spans_pieces_and_reassembles);
    RUN_TEST(test_an_empty_marker_is_one_piece);
    RUN_TEST(test_a_marker_never_goes_out_by_half);
    RUN_TEST(test_a_long_marker_is_cut_at_the_limit);
    RUN_TEST(test_mixed_up_pieces_are_not_a_marker);
    RUN_TEST(test_first_seen_is_written_and_a_repeat_is_not);
    RUN_TEST(test_an_all_zero_frame_is_still_first_seen);
    RUN_TEST(test_a_length_change_is_a_change);
    RUN_TEST(test_same_id_on_two_buses_or_ext_is_two_ids);
    RUN_TEST(test_an_unchanged_id_gets_a_keyframe_every_5_s);
    RUN_TEST(test_keyframes_survive_the_millis_wrap);
    RUN_TEST(test_stats_recover_the_rate_of_skipped_repeats);
    RUN_TEST(test_a_full_table_writes_every_frame_and_counts_it);
    RUN_TEST(test_reset_forgets_every_id);
    RUN_TEST(test_rekey_writes_each_id_again_and_keeps_its_counts);
    RUN_TEST(test_a_change_only_stream_reproduces_every_change);
    return UNITY_END();
}
