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

// ---- v2: segments (docs/design/stabilisation-1-12.md S4) --------------------------

#include <stdio.h>
#include <vector>

static SegEncoder::Mem s_mem;
static SegDecoder      s_dec;   // 3.5 kB of table: not on the stack

struct RecList { std::vector<Rec> r; };
static void into(const Rec& r, void* ctx) { ((RecList*)ctx)->r.push_back(r); }

static Rec mkRec(uint8_t kind, uint32_t t, uint32_t w, std::initializer_list<uint8_t> bytes)
{
    Rec r;
    r.t = t; r.w = w; r.kind = kind; r.len = (uint8_t)bytes.size();
    uint8_t i = 0;
    for (uint8_t b : bytes) r.data[i++] = b;
    return r;
}

static Rec statsRec(uint32_t t, uint32_t w, uint32_t count, uint16_t ms, uint16_t ch)
{
    Rec r = u32Rec(STATS, t, w, count);
    r.len = 8;
    r.data[4] = (uint8_t)ms; r.data[5] = (uint8_t)(ms >> 8);
    r.data[6] = (uint8_t)ch; r.data[7] = (uint8_t)(ch >> 8);
    return r;
}

static void assertSame(const Rec& a, const Rec& b, size_t i)
{
    char m[48];
    snprintf(m, sizeof(m), "record %u", (unsigned)i);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.t, b.t, m);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(a.w, b.w, m);
    TEST_ASSERT_EQUAL_MESSAGE(a.kind, b.kind, m);
    TEST_ASSERT_EQUAL_MESSAGE(a.len, b.len, m);
    if (a.len) TEST_ASSERT_EQUAL_MEMORY_MESSAGE(a.data, b.data, a.len > 8 ? 8 : a.len, m);
}

static Header segHeader(uint32_t seq, uint32_t t0)
{
    Header h;
    h.session = 5; h.part = (uint16_t)seq; h.seq = seq; h.t0 = t0;
    h.bitrate[0] = 500000; h.boot = 3;
    memcpy(h.build, "golden", 7);
    return h;
}

// One of each kind, deltas of every width (negative too), an extended id on
// bus 1, a length change: the fixture tools/can_dump.py's selftest encodes
// byte for byte (GOLDEN_V2 there).
static std::vector<Rec> goldenRecs()
{
    std::vector<Rec> v;
    v.push_back(frame(1000, 0x123, 0, {1, 2, 3}));
    v.push_back(frame(1005, 0x123, 0, {1, 9, 3}));
    Rec k = frame(6005, 0x123, 0, {1, 9, 3}); k.kind = KEYFRAME; v.push_back(k);
    v.push_back(statsRec(6005, idWord(0x123, false, 0), 20, 1000, 1));
    v.push_back(frame(6000, 0x18DAF110, 1, {0xAA}, true));          // dt -5
    v.push_back(frame(6001, 0x123, 0, {1, 9}));                        // length change
    Rec mk[2];
    TEST_ASSERT_EQUAL(2, markerRecs(1, "hi there!", 6002, mk, 2));
    v.push_back(mk[0]); v.push_back(mk[1]);
    v.push_back(u32Rec(GAP, 6003, 0, 3));
    v.push_back(u32Rec(GAP, 6003, 1, 1200));
    v.push_back(u32Rec(BITRATE, 6004, (uint32_t)1 << BUS_SHIFT, 250000));
    Rec md = u32Rec(MODE, 6004, 0, MODE_FULL); md.len = 1; v.push_back(md);
    v.push_back(statsRec(7005, idWord(0x123, false, 0), 300, 1000, 0));
    return v;
}

static const uint8_t GOLDEN_V2[] = {
#include "golden_v2.inc"
};

static size_t encodeAll(const std::vector<Rec>& v, uint8_t* buf, size_t cap, uint32_t seq, uint32_t t0)
{
    SegEncoder e;
    e.begin(&s_mem);
    TEST_ASSERT_TRUE(e.open(buf, cap, segHeader(seq, t0)));
    for (const Rec& r : v) TEST_ASSERT_TRUE(e.add(r));
    return e.size();
}

static void test_v2_header_round_trips_and_v1_still_reads()
{
    Header h = segHeader(77, 123456);
    h.segLen = 4321; h.records = 99; h.wallSec = 1760000000; h.wallAtMs = 10;
    uint8_t b[HEADER2_SIZE];
    encodeHeader2(h, b);
    TEST_ASSERT_EQUAL_MEMORY("MOCAP2", b, 6);
    TEST_ASSERT_EQUAL(80, b[6]);
    TEST_ASSERT_EQUAL(0, b[15]);
    Header g;
    TEST_ASSERT_TRUE(decodeHeader(b, sizeof(b), g));
    TEST_ASSERT_EQUAL(2, g.version);
    TEST_ASSERT_EQUAL(80, g.headerLen);
    TEST_ASSERT_EQUAL(123456, g.t0);
    TEST_ASSERT_EQUAL(4321, g.segLen);
    TEST_ASSERT_EQUAL(77, g.seq);
    TEST_ASSERT_EQUAL(99, g.records);
    TEST_ASSERT_EQUAL(5, g.session);
    TEST_ASSERT_EQUAL(1760000000u, g.wallSec);
    TEST_ASSERT_FALSE(decodeHeader(b, HEADER2_SIZE - 1, g));   // v2 needs its 80

    uint8_t v1[HEADER_SIZE];
    encodeHeader(h, v1);
    TEST_ASSERT_TRUE(decodeHeader(v1, sizeof(v1), g));
    TEST_ASSERT_EQUAL(1, g.version);
    TEST_ASSERT_EQUAL(64, g.headerLen);
}

static void test_v2_every_kind_round_trips()
{
    const std::vector<Rec> v = goldenRecs();
    static uint8_t buf[4096];
    const size_t n = encodeAll(v, buf, sizeof(buf), 7, 1000);
    RecList got;
    Header h;
    uint32_t recs = 0;
    TEST_ASSERT_EQUAL(n, s_dec.segment(buf, n, h, into, &got, &recs));
    TEST_ASSERT_EQUAL(v.size(), recs);
    TEST_ASSERT_EQUAL(v.size(), h.records);
    TEST_ASSERT_EQUAL(n, h.segLen);
    for (size_t i = 0; i < v.size(); ++i) assertSame(v[i], got.r[i], i);
    char text[32];
    TEST_ASSERT_EQUAL(9, markerText(&got.r[6], 2, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("hi there!", text);
}

static void test_v2_bytes_are_the_golden_ones()
{
    static uint8_t buf[4096];
    const size_t n = encodeAll(goldenRecs(), buf, sizeof(buf), 7, 1000);
    TEST_ASSERT_EQUAL(sizeof(GOLDEN_V2), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(GOLDEN_V2, buf, n);
}

// A segment cut at any byte decodes to a prefix of what went in, and to all
// of it at its full length: v1's promise, kept.
static void test_v2_a_segment_cut_short_decodes_to_its_last_whole_record()
{
    const std::vector<Rec> v = goldenRecs();
    static uint8_t buf[4096];
    const size_t n = encodeAll(v, buf, sizeof(buf), 7, 1000);
    size_t last = 0;
    for (size_t cut = HEADER2_SIZE; cut <= n; ++cut) {
        RecList got;
        Header h;
        s_dec.segment(buf, cut, h, into, &got);
        TEST_ASSERT_TRUE(got.r.size() >= last);
        last = got.r.size();
        for (size_t i = 0; i < got.r.size(); ++i) assertSame(v[i], got.r[i], i);
    }
    TEST_ASSERT_EQUAL(v.size(), last);
    Header h;
    TEST_ASSERT_EQUAL(0, s_dec.segment(buf, HEADER2_SIZE - 1, h, nullptr, nullptr));
}

// The second segment of a stream decodes on its own (the first one lost):
// its ids are in its table and their first records carry the whole data.
static void test_v2_a_segment_decodes_without_the_one_before()
{
    SegEncoder e;
    e.begin(&s_mem);
    static uint8_t a[4096], b[4096];
    TEST_ASSERT_TRUE(e.open(a, sizeof(a), segHeader(1, 0)));
    for (uint32_t t = 0; t < 100; t += 10) {
        TEST_ASSERT_TRUE(e.add(frame(t, 0x100, 0, {1, 2, 3, (uint8_t)t})));
        TEST_ASSERT_TRUE(e.add(frame(t, 0x200, 0, {7, 7})));
    }
    TEST_ASSERT_TRUE(e.open(b, sizeof(b), segHeader(2, 100)));
    TEST_ASSERT_EQUAL(2, e.ids());
    std::vector<Rec> want;
    want.push_back(frame(100, 0x200, 0, {7, 7}));
    want.push_back(frame(105, 0x100, 0, {1, 2, 3, 0x64}));
    want.push_back(frame(110, 0x100, 0, {1, 2, 3, 0x6E}));
    for (const Rec& r : want) TEST_ASSERT_TRUE(e.add(r));
    RecList got;
    Header h;
    TEST_ASSERT_EQUAL(e.size(), s_dec.segment(b, e.size(), h, into, &got));
    TEST_ASSERT_EQUAL(2, h.seq);
    TEST_ASSERT_EQUAL(want.size(), got.r.size());
    for (size_t i = 0; i < want.size(); ++i) assertSame(want[i], got.r[i], i);
    // The first carries the whole data: F_FULL, then a delta.
    const size_t rec0 = HEADER2_SIZE + 1 + 2 * 4;
    TEST_ASSERT_TRUE(b[rec0] & F_FULL);
}

// More ids than the table: the first 255 get an index, the rest an escape
// every time, and all of them decode.
static void test_v2_ids_past_the_table_escape_every_time()
{
    SegEncoder e;
    e.begin(&s_mem);
    static uint8_t buf[32768];
    TEST_ASSERT_TRUE(e.open(buf, sizeof(buf), segHeader(1, 0)));
    std::vector<Rec> v;
    for (int round = 0; round < 3; ++round)
        for (uint32_t id = 0; id < 300; ++id)
            v.push_back(frame((uint32_t)round * 1000 + id, 0x400 + id, 0, {(uint8_t)id, (uint8_t)round}));
    for (const Rec& r : v) TEST_ASSERT_TRUE(e.add(r));
    TEST_ASSERT_EQUAL(SEG_IDS, e.ids());
    RecList got;
    Header h;
    s_dec.segment(buf, e.size(), h, into, &got);
    TEST_ASSERT_EQUAL(v.size(), got.r.size());
    for (size_t i = 0; i < v.size(); ++i) assertSame(v[i], got.r[i], i);
    // A full segment refuses the record instead of writing half of it.
    SegEncoder f;
    f.begin(&s_mem);
    static uint8_t small[SEG_HEAD_MAX + REC2_MAX + 40];
    TEST_ASSERT_TRUE(f.open(small, sizeof(small), segHeader(1, 0)));
    size_t added = 0;
    while (f.add(frame(added, 0x7FF, 0, {1, 2, 3, 4, 5, 6, 7, (uint8_t)added}))) ++added;
    TEST_ASSERT_TRUE(added > 0);
    TEST_ASSERT_TRUE(f.size() <= sizeof(small));
    RecList g2;
    s_dec.segment(small, f.size(), h, into, &g2);
    TEST_ASSERT_EQUAL(added, g2.r.size());
}

// Segments back to back (a spilled file, a fetched session): one stream.
static void test_v2_segments_back_to_back_are_one_stream()
{
    const std::vector<Rec> v = goldenRecs();
    static uint8_t buf[8192];
    const size_t a = encodeAll(v, buf, 4096, 1, 1000);
    SegEncoder e;
    e.begin(&s_mem);
    TEST_ASSERT_TRUE(e.open(buf + a, 4096, segHeader(2, 1000)));
    for (const Rec& r : v) TEST_ASSERT_TRUE(e.add(r));
    const size_t n = a + e.size();
    RecList got;
    uint32_t segs = 0;
    TEST_ASSERT_EQUAL(2 * v.size(), s_dec.stream(buf, n, into, &got, &segs));
    TEST_ASSERT_EQUAL(2, segs);
    // The second cut short: the first whole, the second up to its cut.
    got.r.clear();
    const uint32_t cut = s_dec.stream(buf, a + (e.size() - 30), into, &got, &segs);
    TEST_ASSERT_EQUAL(2, segs);
    TEST_ASSERT_TRUE(cut > v.size() && cut < 2 * v.size());
    for (size_t i = 0; i < got.r.size(); ++i) assertSame(v[i % v.size()], got.r[i], i);
}

// ---- the replay: a car-like stream, v1 against v2 ------------------------------
// No v1 capture from the car exists on this machine (the megaopen-logs clone
// has none), so the stream is synthesised from the car's own numbers (boot
// 292, [cap] lines): ~830 frames/s, 45 ids, about half of them changing, at
// 10 ms .. 1 s. The bytes per record are printed for the report.
namespace replay
{
    uint32_t s_rng = 12345;
    uint32_t rnd() { s_rng = s_rng * 1103515245u + 12345u; return s_rng >> 8; }

    struct Car {
        uint32_t id, periodMs, next;
        uint8_t  len, kind;   // kind: 0 constant, 1 counter+checksum, 2 analog, 3 slow
        uint8_t  data[8];
        uint8_t  ctr;
    };

    void step(Car& c)
    {
        switch (c.kind) {
            case 1:   // a rolling counter in byte 0's low nibble, a checksum in the last
                c.ctr = (uint8_t)((c.ctr + 1) & 0xF);
                c.data[0] = (uint8_t)((c.data[0] & 0xF0) | c.ctr);
                if (rnd() % 8 == 0) c.data[2] = (uint8_t)(c.data[2] + (rnd() % 3) - 1);
                c.data[c.len - 1] = (uint8_t)(c.data[0] ^ c.data[1] ^ c.data[2] ^ 0x5A);
                break;
            case 2:   // an analog value (rpm, speed, pedal): noisy low byte, slow high byte
                if (rnd() % 3) {
                    c.data[1] = (uint8_t)(c.data[1] + (rnd() % 5) - 2);
                    if (rnd() % 40 == 0) c.data[0] = (uint8_t)(c.data[0] + 1);
                }
                break;
            case 3:   // fuel, temperatures: a change now and then
                if (rnd() % 200 == 0) c.data[rnd() % c.len] ^= 1;
                break;
            default:
                break;
        }
    }
}

struct Counts { size_t frames = 0, stats = 0, other = 0; };

static void test_v2_replay_of_a_car_like_stream()
{
    using namespace replay;
    std::vector<Car> cars;
    const struct { int n; uint32_t period; uint8_t kinds[10]; } plan[] = {
        {4, 10,   {1, 2, 2, 1}},
        {6, 20,   {1, 1, 2, 0, 0, 1}},
        {8, 100,  {1, 2, 0, 0, 3, 3, 1, 0}},
        {10, 200, {0, 0, 3, 3, 1, 0, 3, 0, 2, 0}},
        {10, 500, {0, 3, 0, 3, 0, 1, 0, 3, 0, 0}},
        {7, 1000, {0, 3, 0, 0, 3, 0, 0}},
    };
    uint32_t id = 0x100;
    for (const auto& p : plan)
        for (int i = 0; i < p.n; ++i) {
            Car c = {};
            c.id = id; id += 0x17;
            c.periodMs = p.period;
            c.next = rnd() % p.period;
            c.len = (uint8_t)(rnd() % 3 == 0 ? 6 : 8);
            c.kind = p.kinds[i];
            for (uint8_t k = 0; k < 8; ++k) c.data[k] = k < c.len ? (uint8_t)rnd() : 0;
            cars.push_back(c);
        }
    TEST_ASSERT_EQUAL(45, (int)cars.size());

    // The frames the bus carries, ten minutes of them.
    std::vector<Rec> bus;
    const uint32_t SPAN = 10 * 60 * 1000;
    for (uint32_t t = 0; t < SPAN; ++t)
        for (Car& c : cars) {
            if (t < c.next) continue;
            c.next += c.periodMs;
            step(c);
            Rec r;
            r.t = t; r.w = idWord(c.id, false, 0); r.kind = FRAME; r.len = c.len;
            memcpy(r.data, c.data, 8);
            bus.push_back(r);
        }

    // v1 as the board wrote it: change filter, stats each second, 18 bytes a
    // record (a 64-byte header per 4 MB file).
    static ChangeFilter::Entry table[1024];
    struct Out { std::vector<Rec>* v; static void put(const Rec& r, void* c) { ((Out*)c)->v->push_back(r); } };
    size_t v1Recs = 0;
    {
        ChangeFilter f;
        f.begin(table, 1024);
        f.startStats(0);
        std::vector<Rec> v1;
        Out o{&v1};
        for (const Rec& fr : bus) {
            Rec r = fr;
            const uint8_t k = f.onFrame(r);
            if (k != ChangeFilter::SKIP) { r.kind = k; v1.push_back(r); }
            if (f.statsDue(r.t)) f.emitStats(r.t, Out::put, &o);
        }
        v1Recs = v1.size();
    }

    // v2 as the recorder writes it: the same filter, each segment rekeyed at
    // its open (every id written whole again) and sealed at 64 kB or 60 s.
    static uint8_t seg[SEG_MAX];
    std::vector<uint8_t> v2;
    std::vector<Rec> wrote;
    SegEncoder e;
    e.begin(&s_mem);
    ChangeFilter f;
    f.begin(table, 1024);
    f.startStats(0);
    uint32_t seq = 0, segs = 0;
    Counts cnt;
    auto seal = [&]() { v2.insert(v2.end(), seg, seg + e.size()); ++segs; e.close(); };
    auto put = [&](const Rec& r) {
        if (e.isOpen() && (uint32_t)(r.t - e.t0()) >= SEG_MS) seal();
        // Each open rekeys the filter, as the recorder does: every id is
        // written whole again at its next frame.
        if (!e.isOpen()) { f.rekey(); TEST_ASSERT_TRUE(e.open(seg, sizeof(seg), segHeader(++seq, r.t))); }
        if (!e.add(r)) {
            seal();
            f.rekey();
            TEST_ASSERT_TRUE(e.open(seg, sizeof(seg), segHeader(++seq, r.t)));
            TEST_ASSERT_TRUE(e.add(r));
        }
        wrote.push_back(r);
        if (r.kind == STATS) ++cnt.stats; else if (r.kind <= KEYFRAME) ++cnt.frames; else ++cnt.other;
    };
    struct PutStats { decltype(put)* p; static void go(const Rec& r, void* c) { (*((PutStats*)c)->p)(r); } } ps{&put};
    for (const Rec& fr : bus) {
        Rec r = fr;
        const uint8_t k = f.onFrame(r);
        if (k != ChangeFilter::SKIP) { r.kind = k; put(r); }
        if (f.statsDue(r.t)) f.emitStats(r.t, PutStats::go, &ps);
    }
    if (e.isOpen()) seal();

    RecList back;
    uint32_t nSegs = 0;
    TEST_ASSERT_EQUAL(wrote.size(), s_dec.stream(v2.data(), v2.size(), into, &back, &nSegs));
    TEST_ASSERT_EQUAL(segs, nSegs);
    for (size_t i = 0; i < wrote.size(); ++i) assertSame(wrote[i], back.r[i], i);

    const double v1Bytes = (double)v1Recs * REC_SIZE + HEADER_SIZE;
    const double perRec = (double)v2.size() / (double)wrote.size();
    const double perFrame = (double)v2.size() / (double)cnt.frames;
    char msg[300];
    snprintf(msg, sizeof(msg),
             "replay (synthesised): %u frames in 10 min (%u/s, 45 ids); v1 %u records = %.0f kB; "
             "v2 %u records (%u frame/key, %u stats) = %.0f kB in %u segments, headers and tables "
             "included: %.2f B/rec, %.2f B per frame record (v1 18); %.1fx smaller",
             (unsigned)bus.size(), (unsigned)(bus.size() / 600), (unsigned)v1Recs, v1Bytes / 1024,
             (unsigned)wrote.size(), (unsigned)cnt.frames, (unsigned)cnt.stats, v2.size() / 1024.0,
             (unsigned)segs, perRec, perFrame, v1Bytes / (double)v2.size());
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE_MESSAGE(perRec <= 6.0, msg);
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
    RUN_TEST(test_v2_header_round_trips_and_v1_still_reads);
    RUN_TEST(test_v2_every_kind_round_trips);
    RUN_TEST(test_v2_bytes_are_the_golden_ones);
    RUN_TEST(test_v2_a_segment_cut_short_decodes_to_its_last_whole_record);
    RUN_TEST(test_v2_a_segment_decodes_without_the_one_before);
    RUN_TEST(test_v2_ids_past_the_table_escape_every_time);
    RUN_TEST(test_v2_segments_back_to_back_are_one_stream);
    RUN_TEST(test_v2_replay_of_a_car_like_stream);
    return UNITY_END();
}
