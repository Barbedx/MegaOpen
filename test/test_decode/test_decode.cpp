#include <unity.h>
#include <string.h>
#include <initializer_list>
#include "vehicle/CanDecode.h"
#include "vehicle/VehicleState.h"
#include "vehicle/DecodeTable.h"
#include "vehicle/Derived.h"
#include "vehicle/AlertRules.h"
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
    // Every row is a hypothesis until it is measured in the car; the owner
    // has measured doors, boot, lights and indicators (2026-10-20).
    const uint8_t measured[] = {SIG_DOOR_FL, SIG_DOOR_FR, SIG_DOOR_RL, SIG_DOOR_RR, SIG_BOOT,
                                SIG_LIGHT_POS, SIG_LIGHT_DIPPED, SIG_LIGHT_MAIN, SIG_IND_LEFT, SIG_IND_RIGHT};
    for (uint8_t i = 0; i < dec.rows(); ++i) {
        bool m = false;
        for (uint8_t s : measured) m |= dec.row(i).sig == s;
        TEST_ASSERT_EQUAL_MESSAGE(!m, (dec.row(i).flags & DF_HYPOTHESIS) != 0, meta(dec.row(i).sig).name);
    }
}

// ---- the X84 broadcast rows (Renault's v84.05 matrix) ----------------------------

// Each new row against the matrix's own `bN.O/W` (byte from 1, bit offset from
// the MSB): one place where a hand-converted shift could go wrong.
void test_x84_rows_match_the_matrix_positions()
{
    struct Ddt { uint8_t sig; uint16_t id; uint8_t n, o, w; };
    const Ddt want[] = {
        {SIG_THROTTLE, 0x181, 4, 0, 8},      {SIG_BRAKE, 0x181, 6, 6, 2},
        {SIG_CLUTCH, 0x181, 6, 3, 2},        {SIG_NEUTRAL, 0x215, 2, 2, 2},
        {SIG_REVERSE_UPC, 0x215, 2, 0, 2},   {SIG_ACCEL_LONG, 0x2A0, 1, 0, 8},
        {SIG_AC_REQUEST, 0x35D, 1, 7, 1},    {SIG_REAR_DEFROST_REQ, 0x35D, 1, 5, 2},
        {SIG_WIPERS, 0x35D, 3, 0, 3},        {SIG_ROLLING, 0x35D, 5, 1, 1},
        {SIG_SUN_LUX, 0x35D, 6, 0, 8},       {SIG_CLIM_AIR_SET, 0x374, 1, 0, 8},
        {SIG_CLIM_FAN_STATE, 0x374, 2, 1, 2}, {SIG_CLIM_AC_SEL, 0x374, 2, 3, 1},
        {SIG_CLIM_EVAP, 0x374, 3, 0, 8},     {SIG_ENG_TEMP, 0x551, 1, 0, 8},
        {SIG_FUEL_COUNTER, 0x551, 2, 0, 8},  {SIG_FUEL_LOW, 0x5C5, 1, 2, 1},
        {SIG_HANDBRAKE, 0x5C5, 1, 5, 1},     {SIG_FUEL_GAUGE_DIAG, 0x5C5, 1, 6, 2},
        {SIG_FUEL_LEVEL, 0x5C5, 8, 1, 7},    {SIG_LOCKED, 0x60D, 3, 3, 2},
        {SIG_OUT_TEMP, 0x60D, 5, 0, 8},      {SIG_REVERSE, 0x60D, 7, 2, 2},
        {SIG_REAR_DEFROST, 0x625, 1, 7, 1},  {SIG_AC_COMP_UPC, 0x625, 2, 0, 1},
        {SIG_BATT_V, 0x625, 3, 0, 8},        {SIG_OIL_PRESS_SW, 0x625, 4, 0, 1},
        {SIG_BATT_LAMP, 0x625, 4, 2, 1},     {SIG_AIRBAG_FAULT, 0x651, 1, 7, 1},
        {SIG_PASS_AIRBAG_OFF, 0x651, 1, 6, 1}, {SIG_BELT_PASS, 0x651, 2, 0, 2},
        {SIG_PASS_PRESENT, 0x651, 2, 2, 2},  {SIG_BELT_DRV, 0x651, 2, 6, 2},
        {SIG_ROOF_POS, 0x684, 1, 0, 3},      {SIG_ROOF_FAULT, 0x684, 1, 4, 1},
        {SIG_MIL, 0x715, 4, 0, 2},           {SIG_OIL_LEVEL, 0x715, 4, 2, 4},
        {SIG_FUEL_LEVEL_APV, 0x715, 5, 0, 7}, {SIG_SERVICE_KM, 0x715, 6, 0, 8},
    };
    uint8_t n = 0;
    const DecodeRow* rows = CanDecode::defaults(n);
    for (const Ddt& d : want) {
        const DecodeRow* r = nullptr;
        for (uint8_t i = 0; i < n; ++i) if (rows[i].sig == d.sig) r = &rows[i];
        TEST_ASSERT_NOT_NULL_MESSAGE(r, meta(d.sig).name);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(d.id, r->canId, meta(d.sig).name);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, r->numBytes, meta(d.sig).name);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(d.n - 1, r->startByte, meta(d.sig).name);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(8 - d.o - d.w, r->shift, meta(d.sig).name);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE((1u << d.w) - 1, r->mask, meta(d.sig).name);
        // A yes/no read from a 2-bit state is one; a raw 0-3 is not.
        const bool state2 = d.w == 2 && meta(d.sig).boolean;
        TEST_ASSERT_EQUAL_MESSAGE(state2, (r->flags & DF_STATE2) != 0, meta(d.sig).name);
        // Inverted where the matrix's 00 is the yes: belts and the seat.
        const bool inv = d.sig == SIG_BELT_DRV || d.sig == SIG_BELT_PASS || d.sig == SIG_PASS_PRESENT;
        TEST_ASSERT_EQUAL_MESSAGE(inv, (r->flags & DF_STATE2_INV) != 0, meta(d.sig).name);
    }
}

void test_x0r_sample_gives_fuel_odometer_and_handbrake()
{
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    State s;
    // x0r.fr's 0x5C5 sample: 19 L, 50 739 km, handbrake off, no low-fuel warning.
    TEST_ASSERT_EQUAL_UINT8(5, dec.apply(frame(0x5C5, {0x40, 0x00, 0xC6, 0x33, 0x06, 0xB0, 0x00, 0x13}), s, 10));
    TEST_ASSERT_EQUAL_INT32(19, s.get(SIG_FUEL_LEVEL));
    TEST_ASSERT_EQUAL_INT32(50739, s.get(SIG_ODO_CLUSTER));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_HANDBRAKE));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_FUEL_LOW));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_FUEL_GAUGE_DIAG));
    // Handbrake is bit2 (0x04), not x0r's bit3; low fuel is bit5 (0x20).
    dec.apply(frame(0x5C5, {0x24, 0x00, 0xC6, 0x33, 0x06, 0xB0, 0x00, 0x85}), s, 20);
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_HANDBRAKE));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_FUEL_LOW));
    TEST_ASSERT_EQUAL_INT32(5, s.get(SIG_FUEL_LEVEL));        // b7 is not litres
    dec.apply(frame(0x5C5, {0x08, 0, 0, 0, 0, 0, 0, 0}), s, 30);
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_HANDBRAKE));         // bit3 alone: off
}

void test_cluster_backup_battery_and_pedals()
{
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    State s;
    // 0x715: MIL 2 and oil level 9 share d3; 15 L in d4 b7-1; 40 x 250 km.
    dec.apply(frame(0x715, {0, 0, 0, 0xA4, 0x1E, 40}), s, 10);
    TEST_ASSERT_EQUAL_INT32(2, s.get(SIG_MIL));
    TEST_ASSERT_EQUAL_INT32(9, s.get(SIG_OIL_LEVEL));
    TEST_ASSERT_EQUAL_INT32(15, s.get(SIG_FUEL_LEVEL_APV));
    TEST_ASSERT_EQUAL_INT32(10000, s.get(SIG_SERVICE_KM));
    // 0x625: 0xDB x 0.0625 = 13.6875 V -> 1368; oil switch and charge lamp.
    dec.apply(frame(0x625, {0x01, 0x80, 0xDB, 0xA0}), s, 10);
    TEST_ASSERT_EQUAL_INT32(1368, s.get(SIG_BATT_V));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_REAR_DEFROST));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_AC_COMP_UPC));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_OIL_PRESS_SW));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_BATT_LAMP));
    TEST_ASSERT_FALSE(s.has(SIG_AC_COMPRESSOR));               // the 0x28 A/C form stays quiet
    // 0x181: pedal 0x80 = 47.3 %, released 0x10 = 0; clutch b6.3, brake b6.6.
    dec.apply(frame(0x181, {0x1F, 0x40, 0, 0x80, 0, 0x08, 0, 0}), s, 10);
    TEST_ASSERT_EQUAL_INT32(473, s.get(SIG_THROTTLE));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_CLUTCH));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_BRAKE));
    dec.apply(frame(0x181, {0x1F, 0x40, 0, 0x10, 0, 0x01, 0, 0}), s, 20);
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_THROTTLE));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_CLUTCH));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_BRAKE));
    // 0x215: neutral b2.2, reverse b2.0.
    dec.apply(frame(0x215, {0, 0x10, 0, 0}), s, 10);
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_NEUTRAL));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_REVERSE_UPC));
    dec.apply(frame(0x215, {0, 0x40, 0, 0}), s, 20);
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_NEUTRAL));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_REVERSE_UPC));
}

void test_belts_lock_roof_climate_and_dynamics()
{
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    State s;
    // The matrix's 00 is buckled / seated: the signals read 1 for it.
    // 0x41: passenger belt 01 (unbuckled), seat 00 (taken), driver belt 01.
    dec.apply(frame(0x651, {0x02, 0x41}), s, 10);
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_BELT_DRV));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_BELT_PASS));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_PASS_PRESENT));
    // 0x10: both belts buckled, nobody on the passenger seat.
    dec.apply(frame(0x651, {0x02, 0x10}), s, 20);
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_BELT_DRV));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_BELT_PASS));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_PASS_PRESENT));
    // 0xFF: every field 11 (reserved): the last reading stands.
    dec.apply(frame(0x651, {0x02, 0xFF}), s, 30);
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_BELT_DRV));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_PASS_PRESENT));
    TEST_ASSERT_EQUAL_UINT32(20, s.stamp(SIG_BELT_DRV));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_PASS_AIRBAG_OFF));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_AIRBAG_FAULT));
    dec.apply(frame(0x60D, {0, 0, 0x18, 0, 0, 0, 0, 0}), s, 10);
    TEST_ASSERT_EQUAL_INT32(3, s.get(SIG_LOCKED));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_FOG_REAR));          // its neighbour, bit2
    dec.apply(frame(0x684, {0xA8, 0}), s, 10);
    TEST_ASSERT_EQUAL_INT32(5, s.get(SIG_ROOF_POS));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_ROOF_FAULT));
    // 0x374: 62 C blown air, fan state 2, A/C selected, evaporator 3.5 C.
    dec.apply(frame(0x374, {102, 0x50, 87}), s, 10);
    TEST_ASSERT_EQUAL_INT32(62, s.get(SIG_CLIM_AIR_SET));
    TEST_ASSERT_EQUAL_INT32(2, s.get(SIG_CLIM_FAN_STATE));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_CLIM_AC_SEL));
    TEST_ASSERT_EQUAL_INT32(35, s.get(SIG_CLIM_EVAP));
    // 0x35D: A/C request, wipers 3, rolling, 20 000 lux.
    dec.apply(frame(0x35D, {0x01, 0, 0x60, 0, 0x40, 20, 0, 0}), s, 10);
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_AC_REQUEST));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_REAR_DEFROST_REQ));
    TEST_ASSERT_EQUAL_INT32(3, s.get(SIG_WIPERS));
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_ROLLING));
    TEST_ASSERT_EQUAL_INT32(20000, s.get(SIG_SUN_LUX));
    // 0x2A0 at rest: 200 = 0 m/s2, 0x7FFF = 0 g and 0 deg/s; then -1 m/s2.
    dec.apply(frame(0x2A0, {200, 0x7F, 0xFF, 0x7F, 0xFF}), s, 10);
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_ACCEL_LONG));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_ACCEL_LAT));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_YAW_RATE));
    dec.apply(frame(0x2A0, {180, 0x80, 0xF9, 0x7F, 0xEA}), s, 20);
    TEST_ASSERT_EQUAL_INT32(-100, s.get(SIG_ACCEL_LONG));
    TEST_ASSERT_EQUAL_INT32(1000, s.get(SIG_ACCEL_LAT));      // +1.000 g
    TEST_ASSERT_EQUAL_INT32(-42, s.get(SIG_YAW_RATE));
    // 0x551 b1: the raw consumption counter.
    dec.apply(frame(0x551, {130, 77, 0, 0, 0, 0, 0}), s, 10);
    TEST_ASSERT_EQUAL_INT32(90, s.get(SIG_ENG_TEMP));
    TEST_ASSERT_EQUAL_INT32(77, s.get(SIG_FUEL_COUNTER));
}

// ---- verified and age ------------------------------------------------------------

void test_measured_means_every_row_and_not_a_hypothesis()
{
    static DecodeTable t;
    t.factory();
    TEST_ASSERT_TRUE(t.measured(SIG_DOOR_FL));
    TEST_ASSERT_FALSE(t.measured(SIG_FUEL_LEVEL));              // a hypothesis
    TEST_ASSERT_FALSE(t.measured(SIG_TPMS_FL));                 // not bound
    TEST_ASSERT_TRUE(t.bindConst(SIG_DOOR_FR, 1));
    TEST_ASSERT_FALSE(t.measured(SIG_DOOR_FR));                 // a constant is not CAN
    // The page re-saving the same field is the same field; a changed one is not.
    const DecodeRow& fuel = t.row[t.rowOf(SIG_FUEL_LEVEL)];
    DecodeRow same = fuel;
    same.flags = 0;
    TEST_ASSERT_TRUE(DecodeTable::sameField(same, fuel));
    same.shift = 1;
    TEST_ASSERT_FALSE(DecodeTable::sameField(same, fuel));
}

void test_verified_is_one_bit_a_signal()
{
    for (uint8_t s = 0; s < SIG_COUNT; ++s) setVerified(s, false);
    setVerified(SIG_DOOR_FL, true);
    setVerified(SIG_YAW_RATE, true);
    TEST_ASSERT_TRUE(verified(SIG_DOOR_FL));
    TEST_ASSERT_TRUE(verified(SIG_YAW_RATE));
    TEST_ASSERT_FALSE(verified(SIG_DOOR_FR));
    TEST_ASSERT_FALSE(verified(SIG_COUNT));
    setVerified(SIG_DOOR_FL, false);
    TEST_ASSERT_FALSE(verified(SIG_DOOR_FL));
    TEST_ASSERT_TRUE(verified(SIG_YAW_RATE));
}

void test_age_is_never_until_set_and_zero_while_pinned()
{
    State s;
    TEST_ASSERT_EQUAL_UINT32(State::NEVER, s.ageMs(SIG_BATT_V, 5000));
    s.set(SIG_BATT_V, 1368, 4000);
    TEST_ASSERT_EQUAL_UINT32(1000, s.ageMs(SIG_BATT_V, 5000));
    TEST_ASSERT_EQUAL_UINT32(1, s.ageMs(SIG_BATT_V, 4001));
    s.set(SIG_BATT_V, 1368, 0xFFFFFF00u);                       // across the millis() wrap
    TEST_ASSERT_EQUAL_UINT32(0x200, s.ageMs(SIG_BATT_V, 0x100));
    s.pin(SIG_BATT_V, 1200);
    TEST_ASSERT_EQUAL_UINT32(0, s.ageMs(SIG_BATT_V, 0x100));
    TEST_ASSERT_EQUAL_UINT32(State::NEVER, s.ageMs(SIG_COUNT, 0));
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
    TEST_ASSERT_EQUAL_UINT8(4, dec.apply(frame(0x181, {0x1F, 0x40, 0, 0, 0, 0x01, 0, 0}), s, 100));
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
    // The matrix centres the wheel on 0x7FFF (offset -3276.7 deg): 0x7FFF - 100
    // = 10.0 degrees left, in tenths: -100.
    dec.apply(frame(0x0C2, {0x7F, 0x9B, 0, 0}), s, 10);
    TEST_ASSERT_EQUAL_INT32(-100, s.get(SIG_STEERING));
    dec.apply(frame(0x0C2, {0x7F, 0xFF, 0, 0}), s, 20);
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_STEERING));
    dec.apply(frame(0x0C2, {0x80, 0x63, 0, 0}), s, 30);
    TEST_ASSERT_EQUAL_INT32(100, s.get(SIG_STEERING));
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
    // GET /api/map?meta=1's rows, min and max still scaled (the mock places
    // the point), and which signals are derived.
    j += "],\"meta\":[";
    for (uint8_t i = 0; i < SIG_COUNT; ++i) {
        const Meta& m = meta(i);
        snprintf(b, sizeof(b), "%s[\"%s\",\"%s\",%u,%d,%u,%ld,%ld,\"%s\"]", i ? "," : "", m.name, m.unit,
                 (unsigned)m.decimals, m.boolean ? 1 : 0, (unsigned)m.group, (long)m.min, (long)m.max, label(i));
        j += b;
    }
    j += "],\"derived\":[";
    const uint8_t* in = nullptr;
    for (uint8_t i = 0, n = 0; i < SIG_COUNT; ++i)
        if (Derived::inputs(i, in)) j += std::string(n++ ? "," : "") + "\"" + meta(i).name + "\"";
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
    TEST_ASSERT_EQUAL_UINT8(62, a.nRows);
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
    TEST_ASSERT_EQUAL_INT32(-32767, b.row[b.rowOf(SIG_STEERING)].add);
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.load(b.row, b.nRows));
}

// The owner's ✓ on the live page: a hypothesis goes measured, survives the
// NVS blob, can be taken back, and factory() brings the hypothesis back.
void test_a_confirm_flips_the_flag_and_survives_the_blob()
{
    static DecodeTable a, b;
    static uint8_t blob[DecodeTable::BLOB_MAX];
    a.factory();
    TEST_ASSERT_FALSE(a.measured(SIG_ACCEL_LAT));
    TEST_ASSERT_FALSE(DecodeTable::factoryMeasured(SIG_ACCEL_LAT));
    TEST_ASSERT_TRUE(DecodeTable::factoryMeasured(SIG_DOOR_FL));
    TEST_ASSERT_TRUE(a.setMeasured(SIG_ACCEL_LAT, true));
    TEST_ASSERT_TRUE(a.measured(SIG_ACCEL_LAT));
    const int ri = a.rowOf(SIG_ACCEL_LAT);
    const uint8_t keep = a.row[ri].flags | DF_HYPOTHESIS;
    TEST_ASSERT_TRUE(b.unpack(blob, a.pack(blob)));
    TEST_ASSERT_TRUE(b.measured(SIG_ACCEL_LAT));
    TEST_ASSERT_FALSE(b.measured(SIG_ACCEL_LONG));              // only that signal
    TEST_ASSERT_TRUE(b.setMeasured(SIG_ACCEL_LAT, false));      // ↺
    TEST_ASSERT_FALSE(b.measured(SIG_ACCEL_LAT));
    TEST_ASSERT_EQUAL_HEX8(keep, b.row[b.rowOf(SIG_ACCEL_LAT)].flags);   // nothing else moved
    TEST_ASSERT_FALSE(b.setMeasured(SIG_RANGE, true));          // no CAN row: refused
    a.factory();                                                // «Заводські прив'язки»
    TEST_ASSERT_FALSE(a.measured(SIG_ACCEL_LAT));
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

void test_the_caps_are_96_rows_and_16_constants()
{
    static DecodeTable t;
    t.nRows = 0; t.nK = 0;
    DecodeRow r{};
    r.numBytes = 1; r.mask = 0xFF; r.mul = 1; r.div = 1;
    for (uint8_t i = 0; i < DecodeTable::MAX_ROWS; ++i) { r.canId = 0x200 + i; t.row[t.nRows++] = r; t.row[i].sig = SIG_RPM; }
    t.row[0].sig = SIG_BRAKE;
    TEST_ASSERT_EQUAL_UINT8(96, DecodeTable::MAX_ROWS);
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
        // The page edits the extraction and the hypothesis mark; latch and key
        // are the row's own (WebUiMap keeps them from the old row).
        const DecodeRow& o = t.row[i];
        TEST_ASSERT_TRUE(r.canId == o.canId && r.startByte == o.startByte && r.numBytes == o.numBytes &&
                         r.shift == o.shift && r.mask == o.mask && r.mul == o.mul && r.div == o.div &&
                         r.add == o.add && r.flags == (o.flags & (DF_LE | DF_SIGNED | DF_EXT | DF_HYPOTHESIS)));
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

// ---- derived: gear, consumption, range ---------------------------------------------

void test_gear_is_the_nearest_ratio_within_the_band()
{
    const Derived::Gears g;
    TEST_ASSERT_EQUAL_INT8(1, Derived::gearFor(g, 75));
    TEST_ASSERT_EQUAL_INT8(4, Derived::gearFor(g, 280));
    TEST_ASSERT_EQUAL_INT8(5, Derived::gearFor(g, 330));
    TEST_ASSERT_EQUAL_INT8(0, Derived::gearFor(g, 100));        // between 1st and 2nd: no gear
    TEST_ASSERT_EQUAL_INT8(0, Derived::gearFor(g, 500));
    Derived::Gears six;
    six.ratio[5] = 420;
    TEST_ASSERT_EQUAL_INT8(6, Derived::gearFor(six, 415));
}

void test_gear_from_speed_rpm_and_the_switches()
{
    Derived d;
    State s;
    uint32_t t = 100;
    auto drive = [&](int32_t speed, int32_t rpm) {
        t += 20;
        s.set(SIG_SPEED, speed, t);
        s.set(SIG_RPM, rpm, t);
        d.update(s, t);
    };
    drive(5000, 1850);                                          // 50 km/h at 1850 rpm
    TEST_ASSERT_EQUAL_INT32(270, s.get(SIG_GEAR_RATIO));
    TEST_ASSERT_EQUAL_INT32(4, s.get(SIG_GEAR));
    s.set(SIG_CLUTCH, 1, t);
    drive(5000, 1850);
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_GEAR));
    s.set(SIG_CLUTCH, 0, t);
    s.set(SIG_NEUTRAL, 1, t);
    drive(5000, 1850);
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_GEAR));
    s.set(SIG_NEUTRAL, 0, t);
    drive(200, 900);                                            // crawling
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_GEAR));
    s.set(SIG_REVERSE_UPC, 1, t);
    drive(500, 1000);
    TEST_ASSERT_EQUAL_INT32(-1, s.get(SIG_GEAR));
    s.set(SIG_REVERSE_UPC, 0, t);
    drive(2000, 2700);                                          // 7.4: first
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_GEAR));
}

void test_fuel_rate_consumption_trip_and_range()
{
    Derived d;
    State s;
    s.set(SIG_SPEED, 10000, 1);                                 // 100 km/h
    s.set(SIG_FUEL_LEVEL, 40, 1);
    uint8_t c = 250;
    uint32_t t = 1000;
    auto counter = [&](uint8_t step) {
        t += 100;
        c = (uint8_t)(c + step);
        s.set(SIG_FUEL_COUNTER, c, t);
        d.update(s, t);
    };
    counter(0);                                                 // the base
    for (int i = 0; i < 9; ++i) counter(5);
    TEST_ASSERT_FALSE(s.has(SIG_FUEL_RATE));                    // 900 ms: the window is open
    counter(5);                                                 // wraps past 255 on the way
    // 50 counts a second x 80 mm3 = 14.4 L/h; at 100 km/h that is 14.4 L/100km.
    TEST_ASSERT_EQUAL_INT32(144, s.get(SIG_FUEL_RATE));
    TEST_ASSERT_EQUAL_INT32(144, s.get(SIG_CONS_INST));
    TEST_ASSERT_FALSE(s.has(SIG_CONS_AVG));                     // under 5 km
    for (int i = 0; i < 1800; ++i) counter(5);                  // three more minutes: 5 km
    TEST_ASSERT_EQUAL_INT32(144, s.get(SIG_CONS_AVG));
    TEST_ASSERT_EQUAL_INT32(50, s.get(SIG_TRIP_KM));            // 181 s at 100 km/h
    TEST_ASSERT_EQUAL_INT32(72, s.get(SIG_TRIP_FUEL));          // 9050 counts = 0.724 L
    TEST_ASSERT_EQUAL_INT32(277, s.get(SIG_RANGE_CALC));        // 40 L / 14.4
    // A glitch and a silence add nothing.
    const int32_t fuel = s.get(SIG_TRIP_FUEL);
    counter(250);
    t += 5000;
    counter(30);                                                // after 5 s: a new base
    for (int i = 0; i < 10; ++i) counter(0);
    TEST_ASSERT_EQUAL_INT32(fuel, s.get(SIG_TRIP_FUEL));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_FUEL_RATE));
}

void test_derived_signals_name_their_inputs()
{
    const uint8_t* in = nullptr;
    TEST_ASSERT_EQUAL_UINT8(0, Derived::inputs(SIG_FUEL_LEVEL, in));
    TEST_ASSERT_NULL(in);
    TEST_ASSERT_EQUAL_UINT8(3, Derived::inputs(SIG_RANGE_CALC, in));
    TEST_ASSERT_EQUAL_UINT8(SIG_FUEL_LEVEL, in[2]);
    for (uint8_t s = SIG_GEAR; s <= SIG_TRIP_KM; ++s) TEST_ASSERT_TRUE(Derived::inputs(s, in) > 0);
}

// ---- panel alerts --------------------------------------------------------------------

void test_alert_factory_is_the_low_fuel_edge_alone()
{
    AlertRules a;
    a.factory();
    TEST_ASSERT_EQUAL_UINT8(SIG_FUEL_LOW, a.r[0].sig);
    TEST_ASSERT_EQUAL_UINT8(AC_RISES, a.r[0].cond);
    TEST_ASSERT_TRUE(a.r[0].on);
    for (uint8_t i = 1; i < AlertRules::ROWS; ++i) {
        TEST_ASSERT_EQUAL_UINT8(SIG_COUNT, a.r[i].sig);
        TEST_ASSERT_FALSE(a.r[i].on);
    }
}

void test_alert_cells_check_their_ranges_and_round_trip()
{
    AlertRules a;
    a.factory();
    TEST_ASSERT_TRUE(a.set(3, AlertRules::C_SIG, SIG_BATT_V));
    TEST_ASSERT_TRUE(a.set(3, AlertRules::C_COND, AC_BELOW));
    TEST_ASSERT_TRUE(a.set(3, AlertRules::C_THR, 1180));
    TEST_ASSERT_FALSE(a.set(3, AlertRules::C_THR, 5000));        // past batt_v's 16.00
    // A pause under the 5 s floor is raised to it, not refused.
    TEST_ASSERT_TRUE(a.set(3, AlertRules::C_COOLDOWN, 0));
    TEST_ASSERT_EQUAL_INT32(ALERT_COOLDOWN_MIN_S, a.get(3, AlertRules::C_COOLDOWN));
    TEST_ASSERT_TRUE(a.set(3, AlertRules::C_COOLDOWN, -1));
    TEST_ASSERT_EQUAL_INT32(ALERT_COOLDOWN_MIN_S, a.get(3, AlertRules::C_COOLDOWN));
    TEST_ASSERT_FALSE(a.set(3, AlertRules::C_COOLDOWN, 70000));
    TEST_ASSERT_TRUE(a.set(3, AlertRules::C_COOLDOWN, 600));
    TEST_ASSERT_EQUAL_INT32(600, a.get(3, AlertRules::C_COOLDOWN));
    TEST_ASSERT_TRUE(a.set(3, AlertRules::C_ON, 1));
    TEST_ASSERT_FALSE(a.set(3, AlertRules::C_ON, 2));
    TEST_ASSERT_FALSE(a.set(3, AlertRules::C_COND, AC_COUNT));
    TEST_ASSERT_FALSE(a.set(3, AlertRules::C_SIG, SIG_COUNT + 1));
    TEST_ASSERT_FALSE(a.set(AlertRules::ROWS, AlertRules::C_ON, 1));
    TEST_ASSERT_FALSE(a.set(3, AlertRules::COLS, 1));
    TEST_ASSERT_EQUAL_INT32(1180, a.get(3, AlertRules::C_THR));
    TEST_ASSERT_EQUAL_INT32(SIG_BATT_V, a.get(3, AlertRules::C_SIG));

    uint8_t blob[AlertRules::BLOB_BYTES];
    TEST_ASSERT_EQUAL_UINT32(AlertRules::BLOB_BYTES, a.toBlob(blob));
    AlertRules b;
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));
    for (uint8_t r = 0; r < AlertRules::ROWS; ++r)
        for (uint8_t c = 0; c < AlertRules::COLS; ++c) TEST_ASSERT_EQUAL_INT32(a.get(r, c), b.get(r, c));
    blob[0] = 2;
    TEST_ASSERT_FALSE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_FALSE(b.fromBlob(blob, sizeof(blob) - 1));
}

void test_alert_low_fuel_edge_fires_once_and_waits_for_the_glass()
{
    AlertRules a;
    a.factory();
    AlertEval e;
    State s;
    AlertEval::Fire f;
    s.set(SIG_FUEL_LOW, 0, 10);
    e.tick(a, s, 100);
    TEST_ASSERT_FALSE(e.take(a, s, true, f));
    s.set(SIG_FUEL_LOW, 1, 200);                                // the lamp lights
    e.tick(a, s, 200);
    TEST_ASSERT_FALSE(e.take(a, s, false, f));                  // a menu holds the glass: waits
    e.tick(a, s, 300);                                          // still on: no second fire
    TEST_ASSERT_TRUE(e.take(a, s, true, f));
    TEST_ASSERT_EQUAL_UINT8(SIG_FUEL_LOW, f.sig);
    TEST_ASSERT_EQUAL_INT32(1, f.value);
    TEST_ASSERT_EQUAL_UINT32(1, e.fired());
    // Off and on again inside the 300 s cooldown: quiet. After it: again.
    s.set(SIG_FUEL_LOW, 0, 400);  e.tick(a, s, 400);
    s.set(SIG_FUEL_LOW, 1, 500);  e.tick(a, s, 500);
    TEST_ASSERT_FALSE(e.take(a, s, true, f));
    s.set(SIG_FUEL_LOW, 0, 1000); e.tick(a, s, 1000);
    s.set(SIG_FUEL_LOW, 1, 301000); e.tick(a, s, 301000);
    TEST_ASSERT_TRUE(e.take(a, s, true, f));
}

void test_alert_already_on_at_first_sight_counts_as_rising()
{
    AlertRules a;
    a.factory();
    AlertEval e;
    State s;
    AlertEval::Fire f;
    s.set(SIG_FUEL_LOW, 1, 10);                                 // lit at ignition
    e.tick(a, s, 10);
    TEST_ASSERT_TRUE(e.take(a, s, true, f));
}

void test_alert_pending_is_dropped_when_its_condition_clears()
{
    AlertRules a;
    a.factory();
    AlertEval e;
    State s;
    AlertEval::Fire f;
    s.set(SIG_FUEL_LOW, 0, 10);  e.tick(a, s, 10);
    s.set(SIG_FUEL_LOW, 1, 20);  e.tick(a, s, 20);
    TEST_ASSERT_FALSE(e.take(a, s, false, f));                  // glass held
    s.set(SIG_FUEL_LOW, 0, 30);  e.tick(a, s, 30);              // over before the glass was free
    TEST_ASSERT_FALSE(e.take(a, s, true, f));
    TEST_ASSERT_EQUAL_UINT32(1, e.dropped());
}

void test_alert_threshold_has_hysteresis_and_newest_wins()
{
    AlertRules a;
    a.factory();
    a.r[0].on = false;
    // batt_v below 11.80 V at the shortest pause (5 s); the band is
    // 1600/50 = 0.32 V. Readings 6 s apart, so the pause never decides.
    a.set(1, AlertRules::C_SIG, SIG_BATT_V);
    a.set(1, AlertRules::C_COND, AC_BELOW);
    a.set(1, AlertRules::C_THR, 1180);
    a.set(1, AlertRules::C_ON, 1);
    AlertEval e;
    State s;
    AlertEval::Fire f;
    uint32_t t = 0;
    auto at = [&](int32_t v) { t += 6000; s.set(SIG_BATT_V, v, t); e.tick(a, s, t); return e.take(a, s, true, f); };
    TEST_ASSERT_FALSE(at(1250));
    TEST_ASSERT_TRUE(at(1170));
    TEST_ASSERT_FALSE(at(1185));                                // back over, inside the band
    TEST_ASSERT_FALSE(at(1175));                                // under again: not re-armed
    TEST_ASSERT_FALSE(at(1212));                                // past thr + band: re-armed
    TEST_ASSERT_TRUE(at(1160));
    // Two rules firing in one tick: the later row's popup is the one shown.
    a.set(2, AlertRules::C_SIG, SIG_HANDBRAKE);
    a.set(2, AlertRules::C_COND, AC_CHANGES);
    a.set(2, AlertRules::C_ON, 1);
    s.set(SIG_HANDBRAKE, 0, t); e.tick(a, s, t);
    TEST_ASSERT_FALSE(e.take(a, s, true, f));
    t += 6000;
    s.set(SIG_BATT_V, 1300, t);
    e.tick(a, s, t);                                            // re-arms batt_v
    s.set(SIG_BATT_V, 1100, t + 1);
    s.set(SIG_HANDBRAKE, 1, t + 1);
    e.tick(a, s, t + 1);
    TEST_ASSERT_TRUE(e.take(a, s, true, f));
    TEST_ASSERT_EQUAL_UINT8(SIG_HANDBRAKE, f.sig);
    TEST_ASSERT_EQUAL_UINT32(1, e.overwritten());
}

// "above 0" on a yes/no: the way back (0 - band = -1) is past the range, so
// it stops at the range's end and the rule re-arms when the value is 0 again.
void test_alert_threshold_at_the_range_end_still_rearms()
{
    AlertRules a;
    a.factory();
    a.r[0].on = false;
    a.set(1, AlertRules::C_SIG, SIG_FUEL_LOW);
    a.set(1, AlertRules::C_COND, AC_ABOVE);
    a.set(1, AlertRules::C_THR, 0);
    a.set(1, AlertRules::C_ON, 1);
    // And "below 3" on the 0-3 lock state: back at 3, not 4.
    a.set(2, AlertRules::C_SIG, SIG_LOCKED);
    a.set(2, AlertRules::C_COND, AC_BELOW);
    a.set(2, AlertRules::C_THR, 3);
    a.set(2, AlertRules::C_ON, 1);
    AlertEval e;
    State s;
    AlertEval::Fire f;
    uint32_t t = 0;
    auto at = [&](uint8_t sig, int32_t v) {
        t += 6000;
        s.set(sig, v, t);
        e.tick(a, s, t);
        return e.take(a, s, true, f) && f.sig == sig;
    };
    s.set(SIG_LOCKED, 3, 1);
    TEST_ASSERT_FALSE(at(SIG_FUEL_LOW, 0));
    TEST_ASSERT_TRUE(at(SIG_FUEL_LOW, 1));
    TEST_ASSERT_FALSE(at(SIG_FUEL_LOW, 0));                     // re-armed here
    TEST_ASSERT_TRUE(at(SIG_FUEL_LOW, 1));
    TEST_ASSERT_TRUE(at(SIG_LOCKED, 2));
    TEST_ASSERT_FALSE(at(SIG_LOCKED, 3));
    TEST_ASSERT_TRUE(at(SIG_LOCKED, 0));
}

// The pause never goes under 5 s, whatever the table says: an edge rule on
// a moving value gives one popup per 5 s, not one per tick.
void test_alert_cooldown_has_a_five_second_floor()
{
    AlertRules a;
    a.factory();
    a.r[0].on = false;
    a.set(1, AlertRules::C_SIG, SIG_RPM);
    a.set(1, AlertRules::C_COND, AC_CHANGES);
    a.set(1, AlertRules::C_COOLDOWN, 0);
    a.set(1, AlertRules::C_ON, 1);
    TEST_ASSERT_EQUAL_UINT16(ALERT_COOLDOWN_MIN_S, a.r[1].cooldownS);
    a.r[1].cooldownS = 0;                                       // written past set()
    AlertEval e;
    State s;
    for (uint32_t t = 0; t < 12000; t += 100) {                 // rpm moves every tick
        s.set(SIG_RPM, (int32_t)(800 + t / 100), t + 1);
        e.tick(a, s, t + 1);
    }
    // Changes from t = 100 on: fires at 0.1 s, 5.1 s and 10.1 s.
    TEST_ASSERT_EQUAL_UINT32(3, e.fired());
    // An older blob's 0 comes back as the floor.
    uint8_t blob[AlertRules::BLOB_BYTES];
    a.toBlob(blob);
    AlertRules b;
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT16(ALERT_COOLDOWN_MIN_S, b.r[1].cooldownS);
    TEST_ASSERT_EQUAL_UINT16(ALERT_COOLDOWN_MIN_S, AlertRule{}.cooldownS);
}

void test_alert_text_places_the_decimal_point()
{
    char b[48];
    AlertEval::text(b, sizeof(b), SIG_BATT_V, 1180, false);
    TEST_ASSERT_EQUAL_STRING("batt_v: 11.80 V ?", b);
    AlertEval::text(b, sizeof(b), SIG_FUEL_LOW, 1, true);
    TEST_ASSERT_EQUAL_STRING("fuel_low: 1", b);
    AlertEval::text(b, sizeof(b), SIG_LOCKED, 3, true);
    TEST_ASSERT_EQUAL_STRING("locked: 3", b);
    AlertEval::text(b, sizeof(b), SIG_CLIM_EVAP, -15, true);
    TEST_ASSERT_EQUAL_STRING("clim_evap: -1.5 C", b);
}

// A Renault 2-bit state reads 01 yes and 00 no; 10/11 (reserved, invalid)
// must not read as "yes": the signal keeps its last value and ages.
void test_state2_invalid_leaves_the_signal_and_is_counted()
{
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    State s;
    dec.apply(frame(0x181, {0, 0, 0, 0x10, 0, 0x01, 0, 0}), s, 10);   // brake 01
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_BRAKE));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_CLUTCH));
    dec.apply(frame(0x181, {0, 0, 0, 0x10, 0, 0x00, 0, 0}), s, 20);   // brake 00
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_BRAKE));
    dec.apply(frame(0x181, {0, 0, 0, 0x10, 0, 0x1B, 0, 0}), s, 30);   // both 11
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_BRAKE));
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_CLUTCH));
    TEST_ASSERT_EQUAL_UINT32(20, s.stamp(SIG_BRAKE));                  // not refreshed
    TEST_ASSERT_EQUAL_UINT32(2, dec.invalid());
    dec.apply(frame(0x181, {0, 0, 0, 0x10, 0, 0x02, 0, 0}), s, 40);   // brake 10
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_BRAKE));
    TEST_ASSERT_EQUAL_UINT32(3, dec.invalid());
    // 0x60D b7.2/2 reverse: 11 leaves it off, 01 is on.
    dec.apply(frame(0x60D, {0, 0, 0, 0, 0x50, 0, 0x30, 0}), s, 50);
    TEST_ASSERT_EQUAL_INT32(0, s.get(SIG_REVERSE));
    dec.apply(frame(0x60D, {0, 0, 0, 0, 0x50, 0, 0x10, 0}), s, 60);
    TEST_ASSERT_EQUAL_INT32(1, s.get(SIG_REVERSE));
}

void test_invalid_reverse_or_neutral_does_not_change_the_gear()
{
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    Derived d;
    State s;
    uint32_t t = 100;
    auto step = [&](uint8_t b215) {
        t += 20;
        dec.apply(frame(0x215, {0, b215, 0, 0, 0, 0, 0, 0}), s, t);
        dec.apply(frame(0x354, {0x13, 0x88, 0, 0, 0, 0, 0, 0}), s, t);   // 50 km/h
        dec.apply(frame(0x181, {0x39, 0xD0, 0, 0x10, 0, 0, 0, 0}), s, t); // 1850 rpm
        d.update(s, t);
        return s.get(SIG_GEAR);
    };
    TEST_ASSERT_EQUAL_INT32(4, step(0x00));
    TEST_ASSERT_EQUAL_INT32(4, step(0xC0));      // reverse 11: invalid
    TEST_ASSERT_EQUAL_INT32(4, step(0x80));      // reverse 10: reserved
    TEST_ASSERT_EQUAL_INT32(4, step(0x30));      // neutral 11: invalid
    TEST_ASSERT_EQUAL_INT32(4, step(0x20));      // neutral 10: not fitted
    TEST_ASSERT_EQUAL_INT32(0, step(0x10));      // neutral 01
    TEST_ASSERT_EQUAL_INT32(-1, step(0x40));     // reverse 01
}

void test_alert_on_brake_ignores_an_invalid_frame()
{
    AlertRules a;
    a.factory();
    a.r[0].on = false;
    TEST_ASSERT_TRUE(a.set(1, AlertRules::C_SIG, SIG_BRAKE));
    TEST_ASSERT_TRUE(a.set(1, AlertRules::C_COND, AC_RISES));
    TEST_ASSERT_TRUE(a.set(1, AlertRules::C_ON, 1));
    CanDecode dec;
    TEST_ASSERT_TRUE(dec.loadDefaults());
    AlertEval e;
    State s;
    AlertEval::Fire f;
    auto at = [&](uint8_t b5, uint32_t t) {
        dec.apply(frame(0x181, {0, 0, 0, 0x10, 0, b5, 0, 0}), s, t);
        e.tick(a, s, t);
        return e.take(a, s, true, f);
    };
    TEST_ASSERT_FALSE(at(0x00, 100));
    TEST_ASSERT_FALSE(at(0x03, 200));            // 11: no rising edge
    TEST_ASSERT_FALSE(at(0x02, 300));
    TEST_ASSERT_FALSE(at(0x00, 400));
    TEST_ASSERT_TRUE(at(0x01, 500));             // a real press still fires
    TEST_ASSERT_EQUAL_UINT8(SIG_BRAKE, f.sig);
}

int main(int, char**)

{
    UNITY_BEGIN();
    RUN_TEST(test_alert_factory_is_the_low_fuel_edge_alone);
    RUN_TEST(test_alert_cells_check_their_ranges_and_round_trip);
    RUN_TEST(test_alert_low_fuel_edge_fires_once_and_waits_for_the_glass);
    RUN_TEST(test_alert_already_on_at_first_sight_counts_as_rising);
    RUN_TEST(test_alert_pending_is_dropped_when_its_condition_clears);
    RUN_TEST(test_alert_threshold_has_hysteresis_and_newest_wins);
    RUN_TEST(test_alert_text_places_the_decimal_point);
    RUN_TEST(test_alert_threshold_at_the_range_end_still_rearms);
    RUN_TEST(test_alert_cooldown_has_a_five_second_floor);
    RUN_TEST(test_state2_invalid_leaves_the_signal_and_is_counted);
    RUN_TEST(test_invalid_reverse_or_neutral_does_not_change_the_gear);
    RUN_TEST(test_alert_on_brake_ignores_an_invalid_frame);
    RUN_TEST(test_gear_is_the_nearest_ratio_within_the_band);
    RUN_TEST(test_gear_from_speed_rpm_and_the_switches);
    RUN_TEST(test_fuel_rate_consumption_trip_and_range);
    RUN_TEST(test_derived_signals_name_their_inputs);
    RUN_TEST(test_the_blob_round_trips_the_factory_table_and_constants);
    RUN_TEST(test_a_confirm_flips_the_flag_and_survives_the_blob);
    RUN_TEST(test_a_bad_blob_is_refused_whole);
    RUN_TEST(test_the_caps_are_96_rows_and_16_constants);
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
    RUN_TEST(test_x84_rows_match_the_matrix_positions);
    RUN_TEST(test_x0r_sample_gives_fuel_odometer_and_handbrake);
    RUN_TEST(test_cluster_backup_battery_and_pedals);
    RUN_TEST(test_belts_lock_roof_climate_and_dynamics);
    RUN_TEST(test_measured_means_every_row_and_not_a_hypothesis);
    RUN_TEST(test_verified_is_one_bit_a_signal);
    RUN_TEST(test_age_is_never_until_set_and_zero_while_pinned);
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
