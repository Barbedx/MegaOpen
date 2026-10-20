#include <unity.h>
#include <string.h>
#include <initializer_list>
#include "vehicle/CanDecode.h"
#include "vehicle/VehicleState.h"
#include "vehicle/DecodeTable.h"
#include <filesystem>
#include <string>

using namespace Vehicle;

void setUp() {}
void tearDown() {}

static Frame frame(uint32_t id, std::initializer_list<uint8_t> bytes, bool ext = false)
{
    Frame f;
    f.id = id;
    f.extended = ext;
    f.source = Frame::SRC_VH_CAN;
    uint8_t i = 0;
    for (uint8_t b : bytes) if (i < 8) f.data[i++] = b;
    f.len = i;
    return f;
}

// ---- extraction ---------------------------------------------------------------

void test_big_endian_word_with_a_divisor()
{
    // 0x181 rpm: raw / 8. 0x1F40 = 8000 -> 1000 rpm.
    const DecodeRow r = {0x181, 0, 2, 0, 0xFFFF, 1, 8, 0, SIG_RPM, 0, 0};
    const uint8_t d[8] = {0x1F, 0x40};
    int32_t v = -1;
    TEST_ASSERT_TRUE(CanDecode::extract(r, d, 8, v));
    TEST_ASSERT_EQUAL_INT32(1000, v);
}

void test_single_bit_inside_a_24_bit_word()
{
    // 0x60D bit 13 (left indicator) sits in byte 1, bit 5.
    const DecodeRow r = {0x60D, 0, 3, 13, 0x1, 1, 1, 0, SIG_IND_LEFT, 0, 0};
    const uint8_t on[3]  = {0x00, 0x20, 0x00};
    const uint8_t off[3] = {0xFF, 0xDF, 0xFF};     // every other bit set
    int32_t v = -1;
    TEST_ASSERT_TRUE(CanDecode::extract(r, on, 3, v));
    TEST_ASSERT_EQUAL_INT32(1, v);
    TEST_ASSERT_TRUE(CanDecode::extract(r, off, 3, v));
    TEST_ASSERT_EQUAL_INT32(0, v);
}

void test_offset_makes_a_negative_temperature()
{
    const DecodeRow r = {0x60D, 4, 1, 0, 0xFF, 1, 1, -40, SIG_OUT_TEMP, 0, 0};
    const uint8_t d[5] = {0, 0, 0, 0, 30};
    int32_t v = 0;
    TEST_ASSERT_TRUE(CanDecode::extract(r, d, 5, v));
    TEST_ASSERT_EQUAL_INT32(-10, v);
}

void test_signed_field_is_twos_complement_in_the_mask_width()
{
    // A 12-bit signed field: 0xFFF is -1, 0x800 is -2048, 0x7FF is 2047.
    const DecodeRow r = {0x100, 0, 2, 0, 0x0FFF, 1, 1, 0, SIG_STEERING, DF_SIGNED, 0};
    int32_t v = 0;
    const uint8_t a[2] = {0x0F, 0xFF};
    TEST_ASSERT_TRUE(CanDecode::extract(r, a, 2, v));
    TEST_ASSERT_EQUAL_INT32(-1, v);
    const uint8_t b[2] = {0x08, 0x00};
    TEST_ASSERT_TRUE(CanDecode::extract(r, b, 2, v));
    TEST_ASSERT_EQUAL_INT32(-2048, v);
    const uint8_t c[2] = {0x07, 0xFF};
    TEST_ASSERT_TRUE(CanDecode::extract(r, c, 2, v));
    TEST_ASSERT_EQUAL_INT32(2047, v);
}

void test_little_endian_flag_swaps_the_bytes()
{
    const DecodeRow r = {0x100, 2, 2, 0, 0xFFFF, 1, 1, 0, SIG_SPEED, DF_LE, 0};
    const uint8_t d[4] = {0xAA, 0xBB, 0x34, 0x12};
    int32_t v = 0;
    TEST_ASSERT_TRUE(CanDecode::extract(r, d, 4, v));
    TEST_ASSERT_EQUAL_INT32(0x1234, v);
}

void test_short_frame_is_skipped_not_read_past()
{
    const DecodeRow r = {0x60D, 4, 1, 0, 0xFF, 1, 1, -40, SIG_OUT_TEMP, 0, 0};
    const uint8_t d[8] = {1, 2, 3, 4, 99, 6, 7, 8};
    int32_t v = 123;
    TEST_ASSERT_FALSE(CanDecode::extract(r, d, 4, v));     // byte 4 is beyond len 4
    TEST_ASSERT_EQUAL_INT32(123, v);
}

// ---- the table -------------------------------------------------------------------

void test_compiled_defaults_load_and_are_sorted()
{
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    TEST_ASSERT_TRUE(dec.rows() > 10);
    for (uint8_t i = 1; i < dec.rows(); ++i)
        TEST_ASSERT_TRUE(dec.row(i - 1).canId <= dec.row(i).canId);
    // Every row is a hypothesis until it is measured in the car.
    for (uint8_t i = 0; i < dec.rows(); ++i)
        TEST_ASSERT_TRUE(dec.row(i).flags & DF_HYPOTHESIS);
}

void test_unsorted_table_is_refused()
{
    static const DecodeRow rows[] = {
        {0x200, 0, 1, 0, 0xFF, 1, 1, 0, SIG_RPM, 0, 0},
        {0x100, 0, 1, 0, 0xFF, 1, 1, 0, SIG_SPEED, 0, 0},
    };
    CanDecode dec;
    TEST_ASSERT_FALSE(dec.load(rows, 2));
    TEST_ASSERT_EQUAL_UINT8(0, dec.rows());
}

void test_bad_signal_or_width_is_refused()
{
    static const DecodeRow badSig[]   = {{0x100, 0, 1, 0, 0xFF, 1, 1, 0, SIG_COUNT, 0, 0}};
    static const DecodeRow badWidth[] = {{0x100, 6, 4, 0, 0xFF, 1, 1, 0, SIG_RPM, 0, 0}};
    CanDecode dec;
    TEST_ASSERT_FALSE(dec.load(badSig, 1));
    TEST_ASSERT_FALSE(dec.load(badWidth, 1));
}

void test_defaults_decode_speed_rpm_and_doors_into_state()
{
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    State s;
    // 0x354: km/h x100, big-endian. 0x1388 = 5000 = 50.00 km/h.
    TEST_ASSERT_EQUAL_UINT8(1, dec.apply(frame(0x354, {0x13, 0x88, 0, 0, 0, 0, 0, 0}), s, 100));
    TEST_ASSERT_EQUAL_INT32(5000, s.get(SIG_SPEED));
    // 0x181: rpm x8, and b5 bit0 the brake.
    TEST_ASSERT_EQUAL_UINT8(2, dec.apply(frame(0x181, {0x1F, 0x40, 0, 0, 0, 0x01, 0, 0}), s, 100));
    TEST_ASSERT_EQUAL_INT32(1000, s.get(SIG_RPM));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_BRAKE));
    // 0x60D: bit 19 = front-left door, byte 4 = outside temperature + 40.
    dec.apply(frame(0x60D, {0x08, 0x00, 0x00, 0, 55, 0, 0x10, 0}), s, 100);
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_DOOR_FL));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_DOOR_FR));
    TEST_ASSERT_EQUAL_INT32(15, s.get(SIG_OUT_TEMP));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_REVERSE));
    TEST_ASSERT_TRUE(s.has(SIG_DOOR_FL));
    TEST_ASSERT_EQUAL_UINT32(100, s.stamp(SIG_DOOR_FL));
}

void test_steering_bias_gives_a_signed_angle()
{
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    State s;
    // 0x8000 - 100 = 10.0 degrees left, in tenths: -100.
    dec.apply(frame(0x0C2, {0x7F, 0x9C, 0, 0}), s, 10);
    TEST_ASSERT_EQUAL_INT32(-100, s.get(SIG_STEERING));
}

void test_unknown_id_touches_nothing_and_is_counted()
{
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    State s;
    TEST_ASSERT_EQUAL_UINT8(0, dec.apply(frame(0x7FF, {1, 2, 3}), s, 10));
    TEST_ASSERT_EQUAL_UINT32(1, dec.unknown());
    TEST_ASSERT_EQUAL_UINT32(0, dec.hits());
    for (uint8_t i = 0; i < SIG_COUNT; ++i) TEST_ASSERT_FALSE(s.has(i));
}

void test_extended_id_does_not_match_a_standard_row()
{
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    State s;
    // 29-bit 0x354 is not the 11-bit speed frame.
    TEST_ASSERT_EQUAL_UINT8(0, dec.apply(frame(0x354, {0x13, 0x88}, true), s, 10));
    TEST_ASSERT_FALSE(s.has(SIG_SPEED));
}

void test_short_frame_skips_the_rows_it_cannot_fill()
{
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    State s;
    // 0x60D with only 3 bytes: the bit rows decode, the byte 4 and 6 rows do not.
    dec.apply(frame(0x60D, {0x80, 0x00, 0x00}), s, 10);
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_BOOT));
    TEST_ASSERT_FALSE(s.has(SIG_OUT_TEMP));
    TEST_ASSERT_FALSE(s.has(SIG_REVERSE));
    TEST_ASSERT_EQUAL_UINT32(2, dec.short_());
}

void test_value_is_clamped_to_the_signal_range()
{
    static const DecodeRow rows[] = {{0x100, 0, 1, 0, 0xFF, 100, 1, 0, SIG_RPM, 0, 0}};
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.load(rows, 1));
    State s;
    dec.apply(frame(0x100, {0xFF}), s, 10);              // 25500 rpm
    TEST_ASSERT_EQUAL_INT32(meta(SIG_RPM).max, s.get(SIG_RPM));
}

// ---- latch and key ---------------------------------------------------------------

void test_indicator_latch_holds_through_the_blink_gap()
{
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    State s;
    const Frame on  = frame(0x60D, {0x00, 0x20, 0x00, 0, 0, 0, 0, 0});   // bit 13
    const Frame off = frame(0x60D, {0x00, 0x00, 0x00, 0, 0, 0, 0, 0});
    dec.apply(on, s, 1000);
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_IND_LEFT));
    dec.apply(off, s, 1300);                       // inside 500 ms: still on
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_IND_LEFT));
    dec.apply(on, s, 1400);                        // the next blink re-arms it
    dec.apply(off, s, 1800);                       // 400 ms after: still on
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_IND_LEFT));
    dec.apply(off, s, 1901);                       // 501 ms after the last blink
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_IND_LEFT));
    // A non-latched bit in the same frame follows the wire at once.
    dec.apply(frame(0x60D, {0x00, 0x08, 0x00, 0, 0, 0, 0, 0}), s, 2000);   // bit 11 main beam
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_LIGHT_MAIN));
    dec.apply(off, s, 2010);
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_LIGHT_MAIN));
}

static int     g_keyCalls;
static uint8_t g_keySig;
static int32_t g_keyVal;
static void onKey(uint8_t sig, int32_t v, void*) { ++g_keyCalls; g_keySig = sig; g_keyVal = v; }

void test_key_row_posts_changes_only_and_leaves_state_alone()
{
    static const DecodeRow rows[] = {{0x2A0, 0, 1, 0, 0xFF, 1, 1, 0, SIG_MUTE, DF_KEY, 0}};
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.load(rows, 1));
    dec.onKey(onKey, nullptr);
    g_keyCalls = 0;
    State s;
    dec.apply(frame(0x2A0, {0x00}), s, 10);        // no change from rest
    TEST_ASSERT_EQUAL_INT(0, g_keyCalls);
    dec.apply(frame(0x2A0, {0x05}), s, 20);
    TEST_ASSERT_EQUAL_INT(1, g_keyCalls);
    TEST_ASSERT_EQUAL_UINT8(SIG_MUTE, g_keySig);
    TEST_ASSERT_EQUAL_INT32(5, g_keyVal);
    dec.apply(frame(0x2A0, {0x05}), s, 30);        // held: no repeat
    TEST_ASSERT_EQUAL_INT(1, g_keyCalls);
    dec.apply(frame(0x2A0, {0x00}), s, 40);        // released
    TEST_ASSERT_EQUAL_INT(2, g_keyCalls);
    TEST_ASSERT_EQUAL_INT32(0, g_keyVal);
    TEST_ASSERT_FALSE(s.has(SIG_MUTE));
}

void test_pin_still_wins_over_a_decoded_value()
{
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    State s;
    s.pin(SIG_SPEED, 1234);
    dec.apply(frame(0x354, {0x13, 0x88}), s, 10);
    TEST_ASSERT_EQUAL_INT32(1234, s.get(SIG_SPEED));
    TEST_ASSERT_EQUAL_INT32(5000, s.decoded(SIG_SPEED));
}


// ---- the user's map: DecodeTable, its NVS blob, the `can` JSON ---------------

static bool sameRow(const DecodeRow& a, const DecodeRow& b)
{
    return a.canId == b.canId && a.startByte == b.startByte && a.numBytes == b.numBytes &&
           a.shift == b.shift && a.mask == b.mask && a.mul == b.mul && a.div == b.div &&
           a.add == b.add && a.sig == b.sig && a.flags == b.flags && a.holdMs == b.holdMs;
}

// Writes build/decode.json for the simulator: the factory map in /api/map's
// `can` shape (the firmware's own canJson), and the signal names (a field
// with a signal can be bound, fw or not).
static void writeForTheSimulator(const DecodeTable& t)
{
    std::string j = "{\"rows\":[";
    char b[200];
    for (uint8_t i = 0; i < t.nRows; ++i) {
        j += std::string(i ? "," : "") + "{\"sig\":\"" + meta(t.row[i].sig).name + "\",\"can\":";
        DecodeJson::canJson(b, sizeof(b), t.row[i]);
        j += std::string(b) + "}";
    }
    j += "],\"const\":[],\"sigs\":[";
    for (uint8_t i = 0; i < SIG_COUNT; ++i) j += std::string(i ? "," : "") + "\"" + meta(i).name + "\"";
    j += "]}\n";
    std::filesystem::path dir = std::filesystem::current_path() / "build";
    std::filesystem::create_directories(dir);
    FILE* fp = fopen((dir / "decode.json").string().c_str(), "wb");
    TEST_ASSERT_NOT_NULL(fp);
    fwrite(j.data(), 1, j.size(), fp);
    fclose(fp);
}

void test_the_blob_round_trips_the_factory_table_and_constants()
{
    static DecodeTable a, b;
    a.factory();
    TEST_ASSERT_EQUAL_UINT8(23, a.nRows);
    writeForTheSimulator(a);
    TEST_ASSERT_TRUE(a.bindConst(SIG_KEY_ON, 1));
    TEST_ASSERT_TRUE(a.bindConst(SIG_OUT_TEMP, -12));
    static uint8_t blob[DecodeTable::BLOB_MAX];
    const size_t n = a.pack(blob);
    TEST_ASSERT_EQUAL_UINT32(3 + (a.nRows) * DecodeTable::ROW_BYTES + 2 * DecodeTable::CONST_BYTES, n);
    TEST_ASSERT_TRUE(b.unpack(blob, n));
    TEST_ASSERT_EQUAL_UINT8(a.nRows, b.nRows);
    for (uint8_t i = 0; i < a.nRows; ++i) TEST_ASSERT_TRUE(sameRow(a.row[i], b.row[i]));
    TEST_ASSERT_EQUAL_UINT8(2, b.nK);
    TEST_ASSERT_EQUAL_INT32(-12, b.k[b.constOf(SIG_OUT_TEMP)].value);
    // The indicator latch and the steering bias survive the trip.
    const int li = b.rowOf(SIG_IND_LEFT);
    TEST_ASSERT_TRUE(li >= 0 && (b.row[li].flags & DF_LATCH) && b.row[li].holdMs == 500);
    TEST_ASSERT_EQUAL_INT32(-32768, b.row[b.rowOf(SIG_STEERING)].add);
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.load(b.row, b.nRows));
}

void test_a_bad_blob_is_refused_whole()
{
    static DecodeTable a, b;
    static uint8_t blob[DecodeTable::BLOB_MAX + 32];
    a.factory();
    a.bindConst(SIG_RANGE, 420);
    const size_t n = a.pack(blob);
    TEST_ASSERT_FALSE(b.unpack(blob, n - 1));                  // short
    TEST_ASSERT_EQUAL_UINT8(0, b.nRows);                        // and left empty
    blob[0] = 2;  TEST_ASSERT_FALSE(b.unpack(blob, n)); blob[0] = 1;   // another layout
    blob[3 + 19] = SIG_COUNT;                                   // row 0's signal
    TEST_ASSERT_FALSE(b.unpack(blob, n));
    a.pack(blob);
    blob[3 + 5] = 5;                                            // row 0 len 5
    TEST_ASSERT_FALSE(b.unpack(blob, n));
    a.pack(blob);
    blob[n - 5] = a.row[0].sig;                                 // const on a CAN-bound signal
    TEST_ASSERT_FALSE(b.unpack(blob, n));
    a.pack(blob);
    blob[1] = DecodeTable::MAX_ROWS + 1;                        // past the cap
    TEST_ASSERT_FALSE(b.unpack(blob, n));
    TEST_ASSERT_FALSE(b.unpack(nullptr, 0));
}

void test_the_caps_are_48_rows_and_16_constants()
{
    static DecodeTable t;
    t.nRows = 0; t.nK = 0;
    DecodeRow r{};
    r.numBytes = 1; r.mask = 0xFF; r.mul = 1; r.div = 1;
    for (uint8_t i = 0; i < DecodeTable::MAX_ROWS; ++i) { r.canId = 0x200 + i; t.row[t.nRows++] = r; t.row[i].sig = SIG_RPM; }
    t.row[0].sig = SIG_BRAKE;
    TEST_ASSERT_EQUAL_UINT8(48, DecodeTable::MAX_ROWS);
    TEST_ASSERT_FALSE(t.bindCan(SIG_SPEED, r));                 // full: nothing moves
    TEST_ASSERT_EQUAL_UINT8(DecodeTable::MAX_ROWS, t.nRows);
    TEST_ASSERT_TRUE(t.bindCan(SIG_BRAKE, r));                  // rebinding in place fits
    for (uint8_t i = 0; i < DecodeTable::MAX_CONSTS; ++i) TEST_ASSERT_TRUE(t.bindConst((uint8_t)(SIG_AC_ON + i), 1));
    TEST_ASSERT_EQUAL_UINT8(16, t.nK);
    TEST_ASSERT_FALSE(t.bindConst(SIG_TPMS_FL, 1));
    TEST_ASSERT_TRUE(t.bindConst(SIG_AC_ON, 0));                // replacing one fits
    static uint8_t blob[DecodeTable::BLOB_MAX];
    t.unbind(SIG_RPM);
    TEST_ASSERT_TRUE(t.pack(blob) <= DecodeTable::BLOB_MAX);
}

void test_binding_replaces_and_keeps_the_order()
{
    static DecodeTable t;
    t.factory();
    const uint8_t n = t.nRows;
    DecodeRow r{};
    r.canId = 0x100; r.numBytes = 1; r.mask = 0xFF; r.mul = 1; r.div = 1;
    TEST_ASSERT_TRUE(t.bindCan(SIG_SPEED, r));                  // replaces 0x354
    TEST_ASSERT_EQUAL_UINT8(n, t.nRows);
    TEST_ASSERT_EQUAL_UINT32(0x100, t.row[t.rowOf(SIG_SPEED)].canId);
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.load(t.row, t.nRows));                 // still sorted
    TEST_ASSERT_TRUE(t.bindConst(SIG_SPEED, 5000));             // can -> const
    TEST_ASSERT_EQUAL_INT(-1, t.rowOf(SIG_SPEED));
    TEST_ASSERT_EQUAL_UINT8(n - 1, t.nRows);
    t.unbind(SIG_SPEED);                                        // off
    TEST_ASSERT_EQUAL_INT(-1, t.constOf(SIG_SPEED));
}

void test_the_can_object_reads_back_what_it_writes()
{
    static DecodeTable t;
    t.factory();
    char b[200];
    for (uint8_t i = 0; i < t.nRows; ++i) {
        DecodeJson::canJson(b, sizeof(b), t.row[i]);
        JsonDocument d;
        TEST_ASSERT_FALSE(deserializeJson(d, b));
        DecodeRow r{};
        TEST_ASSERT_NULL(DecodeJson::canFrom(d.as<JsonObjectConst>(), r));
        // The page edits the extraction; latch, key and the hypothesis mark
        // are the row's own (WebUiMap keeps latch and key from the old row).
        const DecodeRow& o = t.row[i];
        TEST_ASSERT_TRUE(r.canId == o.canId && r.startByte == o.startByte && r.numBytes == o.numBytes &&
                         r.shift == o.shift && r.mask == o.mask && r.mul == o.mul && r.div == o.div &&
                         r.add == o.add && r.flags == (o.flags & (DF_LE | DF_SIGNED | DF_EXT)));
    }
    const char* bad[] = {
        "{\"id\":\"zz\",\"byte\":0,\"mask\":\"FF\"}",
        "{\"id\":\"0x354\",\"byte\":8,\"mask\":\"FF\"}",
        "{\"id\":\"0x354\",\"byte\":6,\"len\":3,\"mask\":\"FF\"}",
        "{\"id\":\"0x354\",\"byte\":0,\"mask\":\"0\"}",
        "{\"id\":\"0x354\",\"byte\":0,\"mask\":\"FF\",\"div\":0}",
        "{\"id\":\"0x354\",\"byte\":0,\"mask\":\"FF\",\"mul\":1.5}",
        "{\"id\":\"0x354\",\"byte\":0,\"mask\":\"FF\",\"mul\":40000}",
        "{\"id\":\"0x354\",\"byte\":0,\"shift\":32,\"mask\":\"FF\"}",
    };
    for (const char* j : bad) {
        JsonDocument d;
        deserializeJson(d, j);
        DecodeRow r{};
        TEST_ASSERT_NOT_NULL_MESSAGE(DecodeJson::canFrom(d.as<JsonObjectConst>(), r), j);
    }
    // A 29-bit id is extended without being told; "5FD" is 0x5FD.
    JsonDocument d;
    deserializeJson(d, "{\"id\":\"18DAF110\",\"byte\":0,\"mask\":\"FF\"}");
    DecodeRow r{};
    TEST_ASSERT_NULL(DecodeJson::canFrom(d.as<JsonObjectConst>(), r));
    TEST_ASSERT_TRUE(r.flags & DF_EXT);
}

void test_a_bound_row_decodes_into_state()
{
    // Odometer's id carrying speed, by the user's say-so: 0x5FD bytes 1-2, LE.
    static DecodeTable t;
    t.factory();
    JsonDocument d;
    deserializeJson(d, "{\"id\":\"5FD\",\"byte\":1,\"len\":2,\"mask\":\"0xFFFF\",\"le\":true,\"mul\":10}");
    DecodeRow r{};
    TEST_ASSERT_NULL(DecodeJson::canFrom(d.as<JsonObjectConst>(), r));
    TEST_ASSERT_TRUE(t.bindCan(SIG_SPEED, r));
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.load(t.row, t.nRows));
    State s;
    dec.apply(frame(0x5FD, {0x00, 0x34, 0x12}), s, 10);
    TEST_ASSERT_EQUAL_INT32(0x1234 * 10, s.get(SIG_SPEED));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_blob_round_trips_the_factory_table_and_constants);
    RUN_TEST(test_a_bad_blob_is_refused_whole);
    RUN_TEST(test_the_caps_are_48_rows_and_16_constants);
    RUN_TEST(test_binding_replaces_and_keeps_the_order);
    RUN_TEST(test_the_can_object_reads_back_what_it_writes);
    RUN_TEST(test_a_bound_row_decodes_into_state);
    RUN_TEST(test_big_endian_word_with_a_divisor);
    RUN_TEST(test_single_bit_inside_a_24_bit_word);
    RUN_TEST(test_offset_makes_a_negative_temperature);
    RUN_TEST(test_signed_field_is_twos_complement_in_the_mask_width);
    RUN_TEST(test_little_endian_flag_swaps_the_bytes);
    RUN_TEST(test_short_frame_is_skipped_not_read_past);
    RUN_TEST(test_compiled_defaults_load_and_are_sorted);
    RUN_TEST(test_unsorted_table_is_refused);
    RUN_TEST(test_bad_signal_or_width_is_refused);
    RUN_TEST(test_defaults_decode_speed_rpm_and_doors_into_state);
    RUN_TEST(test_steering_bias_gives_a_signed_angle);
    RUN_TEST(test_unknown_id_touches_nothing_and_is_counted);
    RUN_TEST(test_extended_id_does_not_match_a_standard_row);
    RUN_TEST(test_short_frame_skips_the_rows_it_cannot_fill);
    RUN_TEST(test_value_is_clamped_to_the_signal_range);
    RUN_TEST(test_indicator_latch_holds_through_the_blink_gap);
    RUN_TEST(test_key_row_posts_changes_only_and_leaves_state_alone);
    RUN_TEST(test_pin_still_wins_over_a_decoded_value);
    return UNITY_END();
}
