#include <unity.h>
#include <string.h>
#include "vehicle/VehicleState.h"
#include "proto/canbox/Rav4Emitter.h"
#include "proto/canbox/RaiseFramer.h"
#include "proto/canbox/RaiseTargets.h"
#include <initializer_list>

using namespace Vehicle;
using E = Canbox::Rav4Emitter;

void setUp() {}
void tearDown() {}

static uint8_t g_last[64]; static uint8_t g_len; static int g_writes;
static int g_byCmd[256];
// Per-command, because tick() emits several messages in one call and looking at
// g_last afterwards silently tests whichever one happened to be last in the
// table — which is the version string, every time.
static uint8_t g_lenByCmd[256];
static void capture(const uint8_t* f, uint8_t n, void*)
{
    memcpy(g_last, f, n); g_len = n; g_writes++;
    g_byCmd[f[1]]++;                    // f[1] is the command byte
    g_lenByCmd[f[1]] = f[2];            // f[2] is the declared payload length
}
static void resetCapture()
{
    g_writes = 0; g_len = 0;
    memset(g_byCmd, 0, sizeof(g_byCmd));
    memset(g_lenByCmd, 0, sizeof(g_lenByCmd));
}

// The checksum rule, recomputed independently of the framer. Two of the vendor
// document's eleven worked examples print a checksum that does not verify
// against their own payload (speed and odometer — its stated sum for speed,
// 0x1CB, is not the sum of the bytes it lists either). The other nine do, and
// this project reproduces all nine, so those two digits are transcription
// errors in the document rather than a different rule. Where a test below uses
// one of the two, it asserts the DOCUMENT'S PAYLOAD and this computed checksum,
// and says so — silently "fixing" the expectation is how a real disagreement
// would get buried the next time.
static uint8_t expectedChecksum(const uint8_t* frame, uint8_t n)
{
    uint8_t sum = 0;
    for (uint8_t i = 1; i + 1 < n; ++i) sum = (uint8_t)(sum + frame[i]);
    return (uint8_t)(sum ^ 0xFF);
}

// ---- state ------------------------------------------------------------------
void test_pin_beats_can_and_never_goes_stale()
{
    State s;
    s.set(SIG_OUT_TEMP, 5, 1000);
    TEST_ASSERT_EQUAL_INT32(5, s.get(SIG_OUT_TEMP));

    s.pin(SIG_OUT_TEMP, -23);
    TEST_ASSERT_EQUAL_INT32(-23, s.get(SIG_OUT_TEMP));
    TEST_ASSERT_EQUAL_INT32(5, s.decoded(SIG_OUT_TEMP));   // CAN value still visible
    TEST_ASSERT_TRUE(s.fresh(SIG_OUT_TEMP, 9999999, 1000));

    s.unpin(SIG_OUT_TEMP);
    TEST_ASSERT_EQUAL_INT32(5, s.get(SIG_OUT_TEMP));
    TEST_ASSERT_FALSE(s.fresh(SIG_OUT_TEMP, 9999999, 1000));
}

void test_pin_makes_an_unknown_signal_sendable()
{
    State s;
    TEST_ASSERT_FALSE(s.has(SIG_SPEED));
    s.pin(SIG_SPEED, 6000);
    TEST_ASSERT_TRUE(s.has(SIG_SPEED));
}

void test_pin_is_clamped_to_the_signal_range()
{
    State s;
    s.pin(SIG_OUT_TEMP, 500);
    TEST_ASSERT_EQUAL_INT32(87, s.get(SIG_OUT_TEMP));
    s.pin(SIG_OUT_TEMP, -500);
    TEST_ASSERT_EQUAL_INT32(-40, s.get(SIG_OUT_TEMP));
}

// Every signal must have a metadata row, because meta() is INDEXED by the enum:
// a missing row does not fail to compile, it shifts every signal after it and
// the web UI then edits a different one than the label says.
void test_every_signal_has_metadata()
{
    for (uint8_t i = 0; i < SIG_COUNT; ++i) {
        const Meta& m = meta(i);
        TEST_ASSERT_NOT_NULL(m.name);
        TEST_ASSERT_TRUE(m.name[0] != '?');
        TEST_ASSERT_TRUE(m.group < GRP_COUNT);
        TEST_ASSERT_EQUAL_UINT8(i, sigByName(m.name));   // names are unique
    }
}

// ---- emitter ----------------------------------------------------------------
void test_silent_about_data_it_does_not_have()
{
    resetCapture();
    State s;
    E e;
    e.begin(capture, nullptr);
    e.tick(s, 1000);

    // No data is not the same as zero data: not one telemetry message goes out.
    TEST_ASSERT_EQUAL_INT(0, g_byCmd[0x24]);
    TEST_ASSERT_EQUAL_INT(0, g_byCmd[0x28]);
    TEST_ASSERT_EQUAL_INT(0, g_byCmd[0x7D]);
    TEST_ASSERT_EQUAL_INT(0, g_byCmd[0x21]);
    // Nor do the messages the protocol notes say never to emit blind.
    TEST_ASSERT_EQUAL_INT(0, g_byCmd[0x1F]);   // hybrid widget on a Megane
    TEST_ASSERT_EQUAL_INT(0, g_byCmd[0x25]);   // TPMS with no sensors
    // The version string is the exception and is meant to be: it carries no
    // vehicle data and is the only proof-of-life visible on the head unit.
    TEST_ASSERT_EQUAL_INT(1, g_byCmd[0x30]);
}

void test_pinned_temperature_is_emitted_with_the_documented_encoding()
{
    resetCapture();
    State s;
    s.pin(SIG_OUT_TEMP, -23);
    E e;
    e.begin(capture, nullptr);

    TEST_ASSERT_TRUE(e.sendNow(E::MSG_TEMP, s));
    // 2E 28 0C ... payload[5] = (t + 40) * 2 = 34
    TEST_ASSERT_EQUAL_UINT8(0x2E, g_last[0]);
    TEST_ASSERT_EQUAL_UINT8(0x28, g_last[1]);
    TEST_ASSERT_EQUAL_UINT8(12,   g_last[2]);
    TEST_ASSERT_EQUAL_UINT8(34,   g_last[3 + 5]);
}

void test_send_now_refuses_a_message_with_no_data()
{
    State s;
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_FALSE(e.sendNow(E::MSG_SPEED, s));
}

void test_on_change_message_beats_its_interval()
{
    resetCapture();
    State s;
    s.pin(SIG_DOOR_FL, 0);
    E e;
    e.begin(capture, nullptr);

    e.tick(s, 1000);
    TEST_ASSERT_EQUAL_INT(1, g_byCmd[0x24]);
    e.tick(s, 1050);                              // inside the 250 ms interval
    TEST_ASSERT_EQUAL_INT(1, g_byCmd[0x24]);

    s.pin(SIG_DOOR_FL, 1);                        // a door opens
    e.tick(s, 1060);
    TEST_ASSERT_EQUAL_INT(2, g_byCmd[0x24]);
    TEST_ASSERT_EQUAL_UINT8(0x40, g_last[3]);     // left front = b6 on a LHD car
}

void test_on_change_sees_the_fifth_payload_byte()
{
    // 0x1E carries the radar master enable in byte 4; flipping it alone must
    // go out at once, not on the 200 ms timer.
    resetCapture();
    State s;
    s.pin(SIG_RADAR_RL, 100);
    s.pin(SIG_RADAR_ENABLE, 0);
    E e;
    e.begin(capture, nullptr);

    e.tick(s, 1000);
    const int first = g_byCmd[0x1E];
    TEST_ASSERT_EQUAL_INT(1, first);
    e.tick(s, 1050);
    TEST_ASSERT_EQUAL_INT(1, g_byCmd[0x1E]);

    s.pin(SIG_RADAR_ENABLE, 1);
    e.tick(s, 1060);
    TEST_ASSERT_EQUAL_INT(2, g_byCmd[0x1E]);
    TEST_ASSERT_EQUAL_UINT8(0x20, g_last[3 + 4] & 0x20);
}

void test_disabled_emitter_is_silent_but_send_now_still_works()
{
    resetCapture();
    State s;
    s.pin(SIG_RPM, 2500);
    E e;
    e.begin(capture, nullptr);
    e.setEnabled(false);

    e.tick(s, 1000);
    TEST_ASSERT_EQUAL_INT(0, g_writes);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_RPM, s));   // the UI's explicit button
    TEST_ASSERT_EQUAL_INT(1, g_writes);
}

void test_speed_is_little_endian_hundredths()
{
    resetCapture();
    State s;
    s.pin(SIG_SPEED, 6000);                       // 60.00 km/h
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_SPEED, s));
    TEST_ASSERT_EQUAL_UINT8(0x7D, g_last[1]);
    TEST_ASSERT_EQUAL_UINT8(0x03, g_last[3]);
    TEST_ASSERT_EQUAL_UINT8(6000 & 0xFF, g_last[4]);
    TEST_ASSERT_EQUAL_UINT8(6000 >> 8,   g_last[5]);
}

// ---- corrections that came out of the protocol research ---------------------
void test_rpm_goes_on_the_wire_times_four()
{
    resetCapture();
    State s;
    s.pin(SIG_RPM, 2500);
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_RPM, s));
    // 2500 -> 10000 -> 0x2710 LE
    TEST_ASSERT_EQUAL_UINT8(0x10, g_last[4]);
    TEST_ASSERT_EQUAL_UINT8(0x27, g_last[5]);
}

void test_doors_are_keyed_by_side_not_by_role()
{
    State s;
    s.pin(SIG_DOOR_FL, 1);
    // Left front is b6. The common "driver = 0x80" reading is RHD and mirrors
    // every icon in this car.
    TEST_ASSERT_EQUAL_UINT8(0x40, E::doorsMask(s));
    State r;
    r.pin(SIG_DOOR_FR, 1);
    TEST_ASSERT_EQUAL_UINT8(0x80, E::doorsMask(r));
}

// The DEFAULT is len 2 with D1 = 0: the head unit's decoder reads D1 whatever
// the length (b7 = bonnet), so len 1 puts the checksum there and the app shows
// the bonnet open (docs/hu-decoder.md §2a).
void test_doors_defaults_to_length_two()
{
    resetCapture();
    State s;
    s.pin(SIG_DOOR_FL, 1);
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_TRUE(e.doorsWithIgnition());
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_DOORS, s));
    TEST_ASSERT_EQUAL_UINT8(2, g_last[2]);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_last[4]);     // D1: no bonnet bit
    TEST_ASSERT_EQUAL_UINT8(6, g_len);            // 2E 24 02 <mask> <D1> <cs>
}

// The bonnet alone sends the doors frame, in D1 b7 next to the ignition bit;
// len 1 has no D1, so there it is dropped, never put in the checksum's place.
void test_doors_message_carries_the_bonnet()
{
    resetCapture();
    State s;
    s.pin(SIG_BONNET, 1);
    s.pin(SIG_KEY_ON, 1);
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_DOORS, s));
    TEST_ASSERT_EQUAL_UINT8(0x00, g_last[3]);     // D0: no door
    TEST_ASSERT_EQUAL_UINT8(0x82, g_last[4]);     // D1: bonnet b7 + ignition b1
    s.pin(SIG_BONNET, 0);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_DOORS, s));
    TEST_ASSERT_EQUAL_UINT8(0x02, g_last[4]);
}

void test_doors_message_carries_the_ignition_bit()
{
    resetCapture();
    State s;
    s.pin(SIG_KEY_ON, 1);
    E e;
    e.begin(capture, nullptr);
    e.setDoorsWithIgnition(true);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_DOORS, s));
    TEST_ASSERT_EQUAL_UINT8(2, g_last[2]);        // len 2, not 1
    TEST_ASSERT_EQUAL_UINT8(0x02, g_last[4]);     // D1 bit1 = IG
}

// The head unit guards on exact length, so the two readings of 0x24 are not a
// degraded frame and a good one — they are a frame and a DROPPED frame. Which one
// is live has to be switchable at runtime, because the document our working door
// icons came from sends len 1 and the spec that gates the info panels sends len 2.
void test_doors_length_is_switchable()
{
    resetCapture();
    State s;
    s.pin(SIG_DOOR_FL, 1);
    s.pin(SIG_KEY_ON, 1);
    E e;
    e.begin(capture, nullptr);

    e.setDoorsWithIgnition(false);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_DOORS, s));
    TEST_ASSERT_EQUAL_UINT8(1, g_last[2]);
    TEST_ASSERT_EQUAL_UINT8(5, g_len);            // 2E 24 01 <mask> <cs>
    // 2E 24 01 40 -> sum 0x65, ^0xFF = 0x9A
    TEST_ASSERT_EQUAL_UINT8(0x9A, g_last[4]);

    e.setDoorsWithIgnition(true);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_DOORS, s));
    TEST_ASSERT_EQUAL_UINT8(2, g_last[2]);
    TEST_ASSERT_EQUAL_UINT8(6, g_len);
}

void test_speed_carries_average_in_the_trailing_pair()
{
    resetCapture();
    State s;
    s.pin(SIG_SPEED, 12345);
    s.pin(SIG_AVG_SPEED, 20000);
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_SPEED, s));
    TEST_ASSERT_EQUAL_UINT8(0x39, g_last[4]);     // 12345 LE
    TEST_ASSERT_EQUAL_UINT8(0x30, g_last[5]);
    TEST_ASSERT_EQUAL_UINT8(0x20, g_last[6]);     // 20000 LE, was padding before
    TEST_ASSERT_EQUAL_UINT8(0x4E, g_last[7]);
    // The vendor document's worked example for this message is the one that does
    // NOT hold together: it says avg = 200.00 km/h, then writes 10 27 (= 10000,
    // i.e. 100.00) into the average field, and prints a checksum for a sum it
    // also states wrongly. Its speed bytes and its layout are corroborated
    // everywhere else, so only the arithmetic is discarded — the checksum is
    // recomputed here rather than copied.
    TEST_ASSERT_EQUAL_UINT8(expectedChecksum(g_last, g_len), g_last[8]);
    TEST_ASSERT_EQUAL_UINT8(0xA3, g_last[8]);
}

// THE ONE PLACE THE TWO READINGS PRODUCE DIFFERENT BYTES, and the reason this
// test changed. The vendor spec describes a 12-bit field whose D1 high nibble is
// "ignored"; the document that actually drove this head unit gives the worked
// example  -26 deg -> 2E 29 02 E6 FF EF,  whose checksum verifies — a full,
// sign-extended int16 in whole degrees.
//
// Sign-extending satisfies both decoders (a 12-bit reader sees 0xFE6, whose bit
// 11 is the sign, and still reads -26). Zero-extending, which is what this test
// used to assert, satisfies only the 12-bit one and turns every left-hand angle
// into a large positive one for the other.
void test_steering_is_signed_int16_lsb_first()
{
    resetCapture();
    State s;
    s.pin(SIG_STEERING, -260);                    // -26.0 deg
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_STEER, s));

    TEST_ASSERT_EQUAL_UINT8(0x2E, g_last[0]);
    TEST_ASSERT_EQUAL_UINT8(0x29, g_last[1]);
    TEST_ASSERT_EQUAL_UINT8(0x02, g_last[2]);
    TEST_ASSERT_EQUAL_UINT8(0xE6, g_last[3]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, g_last[4]);
    TEST_ASSERT_EQUAL_UINT8(0xEF, g_last[5]);     // the document's own checksum

    // Still true after sign extension: the low 12 bits are the 12-bit form.
    const uint16_t raw = (uint16_t)(g_last[3] | (g_last[4] << 8));
    TEST_ASSERT_EQUAL_UINT16(0x0FE6, raw & 0x0FFF);
}

void test_steering_clamps_and_inverts()
{
    resetCapture();
    State s;
    s.pin(SIG_STEERING, -5400);                   // -540.0 deg, the range limit
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_STEER, s));
    TEST_ASSERT_EQUAL_INT16(-540, (int16_t)(g_last[3] | (g_last[4] << 8)));

    // The head unit's own code negates before clamping; the vendor spec does not.
    // One flag, so the bench answers it instead of the documentation.
    e.setSteeringInverted(true);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_STEER, s));
    TEST_ASSERT_EQUAL_INT16(540, (int16_t)(g_last[3] | (g_last[4] << 8)));
}

void test_trip_is_big_endian_and_history_is_thirteen_bytes()
{
    resetCapture();
    State s;
    s.pin(SIG_TRIP_ELAPSED, 90);                  // minutes, not seconds
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_TRIP, s));
    TEST_ASSERT_EQUAL_UINT8(0x21, g_last[1]);
    TEST_ASSERT_EQUAL_UINT8(7, g_last[2]);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_last[5]);     // BE high byte of 90
    TEST_ASSERT_EQUAL_UINT8(90,   g_last[6]);

    // The range field and the trailing 0x02 are the part BOTH sources attest.
    s.pin(SIG_RANGE, 540);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_TRIP, s));
    TEST_ASSERT_EQUAL_UINT8(0x02, g_last[7]);     // 540 = 0x021C, BE
    TEST_ASSERT_EQUAL_UINT8(0x1C, g_last[8]);
    TEST_ASSERT_EQUAL_UINT8(0x02, g_last[9]);     // unit: km

    s.pin(SIG_FUEL_AVG, 65);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_FUEL_HIST, s));
    TEST_ASSERT_EQUAL_UINT8(0x23, g_last[1]);
    TEST_ASSERT_EQUAL_UINT8(13, g_last[2]);       // the len-3 form is dropped by the HU
    TEST_ASSERT_EQUAL_UINT8(0xFF, g_last[6]);     // unused slots are 0xFFFF
}

void test_instant_fuel_matches_the_worked_example()
{
    resetCapture();
    State s;
    s.pin(SIG_FUEL_INST, 65);                     // 6.5 L/100km, x10
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_FUEL_INST, s));
    // 2e 22 03 02 00 41 97
    const uint8_t want[] = {0x2E, 0x22, 0x03, 0x02, 0x00, 0x41, 0x97};
    TEST_ASSERT_EQUAL_UINT8(sizeof(want), g_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, g_last, sizeof(want));
}

// ---- the new half of the protocol -------------------------------------------

// Both 0x28 forms carry the same command byte and the head unit drops the one
// whose length it did not expect, so they must never both be on the schedule.
void test_the_two_0x28_forms_are_mutually_exclusive()
{
    resetCapture();
    State s;
    s.pin(SIG_OUT_TEMP, 21);
    s.pin(SIG_AC_ON, 1);
    E e;
    e.begin(capture, nullptr);

    // Exactly ONE 0x28 per tick, and it is the selected form. g_lenByCmd, not
    // g_last: tick() emits the whole table and the last frame written is always
    // the version string.
    e.setCmd28(E::Cmd28::OutsideTemp);
    e.tick(s, 1000);
    TEST_ASSERT_EQUAL_INT(1, g_byCmd[0x28]);
    TEST_ASSERT_EQUAL_UINT8(12, g_lenByCmd[0x28]);

    resetCapture();
    E e2;
    e2.begin(capture, nullptr);
    e2.setCmd28(E::Cmd28::AirCon);
    e2.tick(s, 1000);
    TEST_ASSERT_EQUAL_INT(1, g_byCmd[0x28]);
    TEST_ASSERT_EQUAL_UINT8(5, g_lenByCmd[0x28]);
}

void test_aircon_packs_the_documented_bits()
{
    resetCapture();
    State s;
    s.pin(SIG_AC_ON, 1);            // D0 b7
    s.pin(SIG_AC_RECIRC, 1);        // D0 b5
    s.pin(SIG_AC_VENT_FACE, 1);     // D1 b6
    s.pin(SIG_AC_FAN, 5);           // D1 b3-0
    s.pin(SIG_AC_TEMP_DRV, 0x1F);   // HI
    s.pin(SIG_AC_UNIT_F, 1);        // D4 b0
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_AC, s));

    TEST_ASSERT_EQUAL_UINT8(0x28, g_last[1]);
    TEST_ASSERT_EQUAL_UINT8(5,    g_last[2]);
    TEST_ASSERT_EQUAL_UINT8(0xA0, g_last[3]);   // b7 | b5
    TEST_ASSERT_EQUAL_UINT8(0x45, g_last[4]);   // b6 | fan 5
    TEST_ASSERT_EQUAL_UINT8(0x1F, g_last[5]);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_last[6]);   // passenger setpoint unpinned
    TEST_ASSERT_EQUAL_UINT8(0x01, g_last[7]);
}

// Front radar is ordered right-to-left and rear left-to-right. The asymmetry is
// documented, and mirroring it is a bug nobody notices from a code review.
void test_radar_front_is_mirrored_relative_to_rear()
{
    resetCapture();
    State s;
    s.pin(SIG_RADAR_FL, 1); s.pin(SIG_RADAR_FLM, 2);
    s.pin(SIG_RADAR_FRM, 3); s.pin(SIG_RADAR_FR, 4);
    s.pin(SIG_RADAR_RL, 1); s.pin(SIG_RADAR_RLM, 2);
    s.pin(SIG_RADAR_RRM, 3); s.pin(SIG_RADAR_RR, 4);
    s.pin(SIG_RADAR_ENABLE, 1);
    s.pin(SIG_RADAR_VOLUME, 3);
    E e;
    e.begin(capture, nullptr);

    TEST_ASSERT_TRUE(e.sendNow(E::MSG_RADAR_FRONT, s));
    TEST_ASSERT_EQUAL_UINT8(0x1D, g_last[1]);
    TEST_ASSERT_EQUAL_UINT8(4, g_last[3]);      // FR first
    TEST_ASSERT_EQUAL_UINT8(1, g_last[6]);      // FL last

    TEST_ASSERT_TRUE(e.sendNow(E::MSG_RADAR_REAR, s));
    TEST_ASSERT_EQUAL_UINT8(0x1E, g_last[1]);
    TEST_ASSERT_EQUAL_UINT8(1, g_last[3]);      // RL first
    TEST_ASSERT_EQUAL_UINT8(4, g_last[6]);      // RR last
    // b5 is the master enable, and every 0x1D is discarded while it is clear.
    TEST_ASSERT_EQUAL_UINT8(0x23, g_last[7]);   // enable | volume 3
}

// The rear message has to reach the head unit BEFORE the front one, because the
// unit latches the master-enable bit out of it and drops 0x1D until it is set.
// The schedule order is the only thing that guarantees it.
void test_rear_radar_is_scheduled_before_front()
{
    TEST_ASSERT_TRUE(E::MSG_RADAR_REAR < E::MSG_RADAR_FRONT);
}

void test_tpms_unknown_wheels_are_no_data_not_zero()
{
    resetCapture();
    State s;
    s.pin(SIG_TPMS_PRESENT, 1);
    s.pin(SIG_TPMS_SHOW_SPARE, 1);
    s.pin(SIG_TPMS_DIAGRAM, 1);
    s.pin(SIG_TPMS_UNIT, 1);           // PSI
    s.pin(SIG_TPMS_FL, 33); s.pin(SIG_TPMS_FR, 33);
    s.pin(SIG_TPMS_RL, 35); s.pin(SIG_TPMS_RR, 35);
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_TPMS, s));

    // The document's worked example: 2E 25 06 A5 21 21 23 23 FF A8
    const uint8_t want[] = {0x2E, 0x25, 0x06, 0xA5, 0x21, 0x21, 0x23, 0x23, 0xFF, 0xA8};
    TEST_ASSERT_EQUAL_UINT8(sizeof(want), g_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, g_last, sizeof(want));
}

void test_hybrid_stays_silent_until_explicitly_asked_for()
{
    resetCapture();
    State s;
    s.pin(SIG_HYBRID_BATT, 6);
    s.pin(SIG_HYBRID_FLOW, 0x0D);
    E e;
    e.begin(capture, nullptr);
    // Battery and flow alone are not consent: a Megane is not a hybrid, and the
    // frame that draws the widget is the one with hybrid_present in it.
    TEST_ASSERT_FALSE(e.sendNow(E::MSG_HYBRID, s));

    s.pin(SIG_HYBRID_PRESENT, 1);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_HYBRID, s));
    // The document's worked example: 2E 1F 02 86 0D 4B
    const uint8_t want[] = {0x2E, 0x1F, 0x02, 0x86, 0x0D, 0x4B};
    TEST_ASSERT_EQUAL_UINT8(sizeof(want), g_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, g_last, sizeof(want));
}

void test_lights_match_the_worked_example()
{
    resetCapture();
    State s;
    s.pin(SIG_LIGHT_DIPPED, 1);   // 0x40
    s.pin(SIG_LIGHT_POS, 1);      // 0x80
    s.pin(SIG_IND_LEFT, 1);       // 0x10
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_LIGHTS, s));
    // 2E 7D 02 01 D0 AF
    const uint8_t want[] = {0x2E, 0x7D, 0x02, 0x01, 0xD0, 0xAF};
    TEST_ASSERT_EQUAL_UINT8(sizeof(want), g_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, g_last, sizeof(want));
}

void test_odometer_matches_the_worked_example()
{
    resetCapture();
    State s;
    s.pin(SIG_ODOMETER, 123456);
    s.pin(SIG_TRIP1, 340);        // 34.0 km, in 0.1 km
    s.pin(SIG_TRIP2, 2310);       // 231.0 km
    E e;
    e.begin(capture, nullptr);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_ODO, s));
    // The document's payload, verbatim — odo 123456, the constant F2 08, then
    // trip1 34.0 and trip2 231.0 as LE u24 in 0.1 km:
    //   2E 7D 0C 04 40 E2 01 F2 08 54 01 00 06 09 00 <cs>
    // Its printed checksum (0x5D) is the second of the two that do not verify
    // against its own bytes; every field before it is corroborated.
    const uint8_t want[] = {0x2E, 0x7D, 0x0C, 0x04, 0x40, 0xE2, 0x01, 0xF2,
                            0x08, 0x54, 0x01, 0x00, 0x06, 0x09, 0x00};
    TEST_ASSERT_EQUAL_UINT8(sizeof(want) + 1, g_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, g_last, sizeof(want));
    TEST_ASSERT_EQUAL_UINT8(expectedChecksum(g_last, g_len), g_last[15]);
    TEST_ASSERT_EQUAL_UINT8(0xF1, g_last[15]);
}

// The 0x90 responder: the head unit names a command byte and we answer with that
// message. This is how the unit tells us which messages its profile wants.
void test_send_by_cmd_answers_a_request()
{
    resetCapture();
    State s;
    s.pin(SIG_RPM, 2500);
    E e;
    e.begin(capture, nullptr);

    TEST_ASSERT_FALSE(e.sendByCmd(0x1E, s));      // asked for radar, nothing pinned
    TEST_ASSERT_TRUE(e.sendByCmd(0x7D, s));       // rpm is the only 0x7D we can build
    TEST_ASSERT_EQUAL_UINT8(0x7D, g_last[1]);
    TEST_ASSERT_FALSE(e.sendByCmd(0x99, s));      // no builder for this command
}

// 0x28 is two rows in the table with one command byte; the responder must honour
// the selected form rather than putting the other one on the wire behind our back.
void test_send_by_cmd_honours_the_selected_0x28_form()
{
    resetCapture();
    State s;
    s.pin(SIG_OUT_TEMP, 21);
    s.pin(SIG_AC_ON, 1);
    E e;
    e.begin(capture, nullptr);

    e.setCmd28(E::Cmd28::AirCon);
    TEST_ASSERT_TRUE(e.sendByCmd(0x28, s));
    TEST_ASSERT_EQUAL_UINT8(5, g_last[2]);

    e.setCmd28(E::Cmd28::OutsideTemp);
    TEST_ASSERT_TRUE(e.sendByCmd(0x28, s));
    TEST_ASSERT_EQUAL_UINT8(12, g_last[2]);
}

void test_a_disabled_message_leaves_the_schedule_but_not_the_button()
{
    resetCapture();
    State s;
    s.pin(SIG_RPM, 2500);
    E e;
    e.begin(capture, nullptr);
    e.setMsgEnabled(E::MSG_RPM, false);
    e.setMsgEnabled(E::MSG_VERSION, false);

    e.tick(s, 1000);
    TEST_ASSERT_EQUAL_INT(0, g_writes);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_RPM, s));   // explicit is still explicit
}

// preview() must produce exactly what sendNow() transmits, or the page shows one
// thing and the wire carries another — which is worse than showing nothing.
void test_preview_matches_what_is_sent()
{
    resetCapture();
    State s;
    s.pin(SIG_SPEED, 12345);
    s.pin(SIG_AVG_SPEED, 20000);
    E e;
    e.begin(capture, nullptr);

    uint8_t frame[Canbox::MAX_FRAME];
    uint8_t flen = 0;
    TEST_ASSERT_TRUE(e.preview(E::MSG_SPEED, s, frame, flen));
    TEST_ASSERT_EQUAL_INT(0, g_writes);           // a preview transmits nothing

    TEST_ASSERT_TRUE(e.sendNow(E::MSG_SPEED, s));
    TEST_ASSERT_EQUAL_UINT8(flen, g_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, g_last, flen);
}

void test_every_message_has_a_name_and_a_command()
{
    TEST_ASSERT_EQUAL_UINT8(E::MSG_COUNT, E::messageCount());
    for (uint8_t i = 0; i < E::messageCount(); ++i) {
        TEST_ASSERT_NOT_NULL(E::messageName(i));
        TEST_ASSERT_TRUE(E::messageName(i)[0] != '?');
        TEST_ASSERT_TRUE(E::messageCmd(i) != 0);
        TEST_ASSERT_TRUE(E::messageInterval(i) > 0);
    }
}


// ---- the uncertain-value block ----------------------------------------------
// Everything the sources disagree about has to be reachable at runtime, because
// it gets resolved in a car and reflashing there means a laptop on the seat.
// These tests are the contract: each knob must actually change the wire bytes.

void test_tuning_survives_a_blob_round_trip()
{
    Canbox::Tuning a;
    a.cmd28 = 1; a.doorsLen = 2; a.steerInvert = 1; a.steerDiv = 1;
    a.steerClamp = 380; a.fuelUnit = 1;
    a.tripUnit = 1; a.tripFields = 0; a.histLen = 3; a.radarInvert = 1;
    for (int i = 0; i < 16; ++i) a.version[i] = "ABCDEFGHIJKLMNOP"[i];
    a.version[16] = '\0';

    uint8_t blob[Canbox::Tuning::BLOB_SIZE];
    a.toBlob(blob);

    Canbox::Tuning b;
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT8(1, b.cmd28);
    TEST_ASSERT_EQUAL_UINT8(2, b.doorsLen);
    TEST_ASSERT_EQUAL_UINT8(1, b.steerInvert);
    TEST_ASSERT_EQUAL_UINT8(1, b.steerDiv);
    TEST_ASSERT_EQUAL_INT16(380, b.steerClamp);
    TEST_ASSERT_EQUAL_UINT8(1, b.fuelUnit);
    TEST_ASSERT_EQUAL_UINT8(1, b.tripUnit);
    TEST_ASSERT_EQUAL_UINT8(0, b.tripFields);
    TEST_ASSERT_EQUAL_UINT8(3, b.histLen);
    TEST_ASSERT_EQUAL_UINT8(1, b.radarInvert);
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOP", b.version);
}

// len 1 was the default until 2026-10-17 and shows the bonnet open: a blob
// written before then holding it becomes 2, once. A blob written since keeps
// a 1 the owner chose.
void test_an_old_doors_length_one_is_migrated_once()
{
    Canbox::Tuning a;
    a.doorsLen = 1;
    uint8_t blob[Canbox::Tuning::BLOB_SIZE];
    a.toBlob(blob);
    Canbox::Tuning b;
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT8(1, b.doorsLen);                      // chosen since: kept

    blob[7] = 4; blob[8] = 0; blob[9] = 1; blob[10] = 0;         // the old rpm x4 / 1
    // (the marker was bytes 7..10; a blob from before has the rpm scale there)
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT8(2, b.doorsLen);                      // old default: migrated
    b.toBlob(blob);                                              // and saved as such
    Canbox::Tuning c;
    TEST_ASSERT_TRUE(c.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT8(2, c.doorsLen);
}

// An OTA rollback reads the kept blob with the OLD layout: bytes 7..10 were
// rpmMul (LE16) and rpmDiv (LE16). The migration marker must still be x4 there,
// or the old build sends rpm 0.
void test_the_migration_marker_reads_as_rpm_times_four_in_the_old_layout()
{
    Canbox::Tuning a;
    uint8_t blob[Canbox::Tuning::BLOB_SIZE];
    a.toBlob(blob);
    const int16_t mul = (int16_t)(blob[7] | (blob[8] << 8));
    const int16_t div = (int16_t)(blob[9] | (blob[10] << 8));
    TEST_ASSERT_TRUE(div != 0);
    TEST_ASSERT_EQUAL_INT32(4, mul / div);
    TEST_ASSERT_EQUAL_INT32(0, mul % div);
    TEST_ASSERT_EQUAL_INT32(10000, (int32_t)((int64_t)2500 * mul / div));   // 2500 rpm on the wire
}

// A stored block from a firmware with a different layout must be REJECTED, not
// reinterpreted. These are protocol bytes: reading someone else's layout would
// put invented values on the wire and look exactly like a head-unit fault.
void test_a_blob_from_another_layout_is_rejected()
{
    Canbox::Tuning a;
    uint8_t blob[Canbox::Tuning::BLOB_SIZE];
    a.toBlob(blob);
    blob[0] = 99;                                   // a layout version we do not know
    Canbox::Tuning b;
    TEST_ASSERT_FALSE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_FALSE(b.fromBlob(blob, sizeof(blob) - 1));
}

// A length that would build a frame the head unit silently drops must not come
// back out of NVS. Falling back to the spec value is visible; a dropped frame is
// a blank screen with no explanation.
void test_an_absurd_history_length_falls_back_to_the_spec_value()
{
    Canbox::Tuning a;
    uint8_t blob[Canbox::Tuning::BLOB_SIZE];
    a.toBlob(blob);
    blob[14] = 200;
    Canbox::Tuning b;
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT8(13, b.histLen);
}

void test_the_steering_knobs_each_change_the_wire()
{
    resetCapture();
    State s;
    s.pin(SIG_STEERING, -4500);                     // -450.0 deg
    E e;
    e.begin(capture, nullptr);
    auto wire = [&]() -> int16_t {
        TEST_ASSERT_TRUE(e.sendNow(E::MSG_STEER, s));
        return (int16_t)(g_last[3] | (g_last[4] << 8));
    };

    Canbox::Tuning t;                               // defaults: div 10, clamp 540
    e.setTuning(t);
    TEST_ASSERT_EQUAL_INT16(-450, wire());

    t.steerClamp = 380; e.setTuning(t);             // the vendor spec's range
    TEST_ASSERT_EQUAL_INT16(-380, wire());

    t.steerClamp = 540; t.steerInvert = 1; e.setTuning(t);
    TEST_ASSERT_EQUAL_INT16(450, wire());

    // divisor 1 is the document's PROSE reading (0.1 deg units). The clamp is
    // what stops it reaching the +/-5400 the same document warns against.
    t.steerInvert = 0; t.steerDiv = 1; e.setTuning(t);
    TEST_ASSERT_EQUAL_INT16(-540, wire());
}

void test_the_unit_bytes_are_settable()
{
    resetCapture();
    State s;
    s.pin(SIG_FUEL_INST, 65);
    s.pin(SIG_RANGE, 540);
    E e;
    e.begin(capture, nullptr);

    Canbox::Tuning t;
    t.fuelUnit = 1;                                 // km/L instead of L/100km
    t.tripUnit = 1;                                 // miles instead of km
    e.setTuning(t);

    TEST_ASSERT_TRUE(e.sendNow(E::MSG_FUEL_INST, s));
    TEST_ASSERT_EQUAL_UINT8(0x01, g_last[3]);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_TRIP, s));
    TEST_ASSERT_EQUAL_UINT8(0x01, g_last[9]);
}

// The one source that drove this head unit reads 0x21's first four bytes as
// padding that is always zero; the vendor spec reads them as average speed and
// elapsed minutes. Both have to be reachable.
void test_trip_fields_can_be_blanked()
{
    resetCapture();
    State s;
    s.pin(SIG_AVG_SPEED, 20000);
    s.pin(SIG_TRIP_ELAPSED, 90);
    s.pin(SIG_RANGE, 540);
    E e;
    e.begin(capture, nullptr);

    Canbox::Tuning t;
    e.setTuning(t);                                 // tripFields = 1
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_TRIP, s));
    TEST_ASSERT_EQUAL_UINT8(0x07, g_last[3]);       // 2000 BE = 0x07D0
    TEST_ASSERT_EQUAL_UINT8(90,   g_last[6]);

    t.tripFields = 0; e.setTuning(t);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_TRIP, s));
    for (uint8_t i = 3; i <= 6; ++i) TEST_ASSERT_EQUAL_UINT8(0x00, g_last[i]);
    TEST_ASSERT_EQUAL_UINT8(0x02, g_last[7]);       // range still there: 540 = 0x021C
    TEST_ASSERT_EQUAL_UINT8(0x1C, g_last[8]);
}

void test_radar_polarity_inverts_but_leaves_no_display_alone()
{
    resetCapture();
    State s;
    s.pin(SIG_RADAR_RL, 0);      // 0 = nothing there, in BOTH readings
    s.pin(SIG_RADAR_RLM, 1);     // nearest, per the spec
    s.pin(SIG_RADAR_RRM, 4);     // farthest
    s.pin(SIG_RADAR_RR, 2);
    E e;
    e.begin(capture, nullptr);

    Canbox::Tuning t;
    t.radarInvert = 1;
    e.setTuning(t);
    TEST_ASSERT_TRUE(e.sendNow(E::MSG_RADAR_REAR, s));
    // 0 stays 0 — inverting it would turn "nothing there" into "touching the
    // bumper", which is the one error worth being careful about here.
    TEST_ASSERT_EQUAL_UINT8(0, g_last[3]);
    TEST_ASSERT_EQUAL_UINT8(4, g_last[4]);
    TEST_ASSERT_EQUAL_UINT8(1, g_last[5]);
    TEST_ASSERT_EQUAL_UINT8(3, g_last[6]);
}

void test_the_version_string_is_settable_and_always_sixteen_bytes()
{
    resetCapture();
    State s;
    E e;
    e.begin(capture, nullptr);

    Canbox::Tuning t;
    for (int i = 0; i < 16; ++i) t.version[i] = (i < 4) ? "ABCD"[i] : ' ';
    t.version[16] = '\0';
    e.setTuning(t);

    TEST_ASSERT_TRUE(e.sendNow(E::MSG_VERSION, s));
    TEST_ASSERT_EQUAL_UINT8(0x30, g_last[1]);
    TEST_ASSERT_EQUAL_UINT8(16,   g_last[2]);
    TEST_ASSERT_EQUAL_UINT8(20,   g_len);           // 2E + cmd + len + 16 + cs
    TEST_ASSERT_EQUAL_UINT8('A',  g_last[3]);
    TEST_ASSERT_EQUAL_UINT8('D',  g_last[6]);
    // Padding is spaces, never whatever was left in the buffer.
    for (uint8_t i = 7; i < 19; ++i) TEST_ASSERT_EQUAL_UINT8(' ', g_last[i]);
}


// ---- RaiseTargets: the page's rows -------------------------------------------
// The page, /api/map and /api/map/test all address a field by its row: an id
// twice, a row naming no signal, or a "sent" row whose message ignores it would
// each be a button that silently does nothing.

void test_raise_targets_ids_are_unique_and_name_real_signals()
{
    namespace RT = Canbox::RaiseTargets;
    TEST_ASSERT_TRUE(RT::count() > 90);
    for (uint8_t i = 0; i < RT::count(); ++i) {
        const Canbox::TargetView t = RT::at(i);
        TEST_ASSERT_NOT_NULL(t.id);
        TEST_ASSERT_TRUE_MESSAGE(t.min <= t.max, t.id);
        // The name is the page's (k.raise.<id>); a row keeps one only when it is
        // the protocol's own word (AUTO, ECO).
        TEST_ASSERT_TRUE_MESSAGE(t.group && *t.group && t.cmd && (*t.cmd || !t.fw), t.id);
        TEST_ASSERT_EQUAL_INT_MESSAGE(i, RT::find(t.id), t.id);          // unique
        if (t.sig < SIG_COUNT) TEST_ASSERT_EQUAL_STRING(meta(t.sig).name, t.id);
        // A field the firmware sends has a signal to hold its value.
        if (t.fw) TEST_ASSERT_TRUE_MESSAGE(t.sig < SIG_COUNT && t.msg < E::MSG_COUNT, t.id);
        else      TEST_ASSERT_EQUAL_UINT8_MESSAGE(Canbox::NO_MSG, t.msg, t.id);
    }
    TEST_ASSERT_EQUAL_INT(-1, RT::find("nope"));
    // The bonnet goes out in 0x24 D1 b7. The battery is the one row not sent:
    // protocol 112 has no field for it, and the page says so (`why`).
    TEST_ASSERT_TRUE(RT::at((uint8_t)RT::find("bonnet")).fw);
    for (uint8_t i = 0; i < RT::count(); ++i) {
        const Canbox::TargetView t = RT::at(i);
        TEST_ASSERT_EQUAL_MESSAGE(t.fw, t.why == nullptr, t.id);
        TEST_ASSERT_TRUE_MESSAGE(t.sig < SIG_COUNT, t.id);
        if (!t.fw) TEST_ASSERT_EQUAL_STRING("batt_v", t.id);
    }
}

void test_every_sent_raise_target_changes_its_message()
{
    // Min and max of the field must give two different frames from its
    // message's builder: the row is wired to the bytes it claims.
    namespace RT = Canbox::RaiseTargets;
    E e;
    e.setExperiments(E::EXP_GEAR);                  // 0x7D 06 is built only with it
    for (uint8_t i = 0; i < RT::count(); ++i) {
        const Canbox::TargetView t = RT::at(i);
        if (!t.fw) continue;
        State s;
        // The fields that gate a message (trips ride on the odometer's 0x7D 04):
        // /api/map/test answers 409 until the gate has a value.
        s.pin(SIG_ODOMETER, 1);
        s.pin(SIG_TPMS_PRESENT, 1);
        s.pin(SIG_HYBRID_PRESENT, 1);
        s.pin(SIG_RADAR_ENABLE, 1);
        uint8_t a[64], b[64], la = 0, lb = 0;
        s.pin(t.sig, t.min);
        TEST_ASSERT_TRUE_MESSAGE(e.preview(t.msg, s, a, la), t.id);
        s.pin(t.sig, t.max);
        TEST_ASSERT_TRUE_MESSAGE(e.preview(t.msg, s, b, lb), t.id);
        TEST_ASSERT_TRUE_MESSAGE(la != lb || memcmp(a, b, la) != 0, t.id);
    }
    // And every signal the emitter reads has a row (only the 8 it does not
    // send are missing). The Mégane broadcast signals from SIG_FUEL_LEVEL on
    // have no Raise field, except the selector probe, SIG_PRND, the bonnet,
    // and the battery (listed, not sent).
    bool listed[SIG_COUNT] = {};
    uint8_t n = 0;
    for (uint8_t i = 0; i < RT::count(); ++i) {
        const uint8_t sg = RT::at(i).sig;
        if (sg < SIG_COUNT && !listed[sg]) { listed[sg] = true; ++n; }
    }
    TEST_ASSERT_EQUAL_UINT8(SIG_FUEL_LEVEL - 7 + 3, n);
    TEST_ASSERT_TRUE(listed[SIG_PRND]);
    TEST_ASSERT_TRUE(listed[SIG_BONNET]);
    TEST_ASSERT_TRUE(listed[SIG_BATT_V]);
}

void test_raise_values_place_the_decimal_point()
{
    namespace RT = Canbox::RaiseTargets;
    char b[16];
    TEST_ASSERT_EQUAL_STRING("47.00", RT::fmt(b, sizeof(b), 4700, 2));
    TEST_ASSERT_EQUAL_STRING("-0.5", RT::fmt(b, sizeof(b), -5, 1));
    TEST_ASSERT_EQUAL_STRING("-40", RT::fmt(b, sizeof(b), -40, 0));
    TEST_ASSERT_EQUAL_STRING("655.35", RT::fmt(b, sizeof(b), 65535, 2));
    int32_t v = 0;
    TEST_ASSERT_TRUE(RT::toInt(47.004, 2, v));
    TEST_ASSERT_EQUAL_INT32(4700, v);
    TEST_ASSERT_TRUE(RT::toInt(-12.3, 1, v));
    TEST_ASSERT_EQUAL_INT32(-123, v);
    TEST_ASSERT_FALSE(RT::toInt(1e12, 0, v));
}

// ---- head-unit experiments: each its own switch, all off by default --------------

static int g_sub09, g_cmd21, g_cmd22, g_cmd23;
static uint8_t g_odo[16];
static void expCapture(const uint8_t* f, uint8_t n, void*)
{
    if (f[1] == 0x7D && f[3] == 0x09) ++g_sub09;
    if (f[1] == 0x7D && f[3] == 0x04 && n <= sizeof(g_odo)) memcpy(g_odo, f, n);
    g_cmd21 += f[1] == 0x21;
    g_cmd22 += f[1] == 0x22;
    g_cmd23 += f[1] == 0x23;
}

void test_experiments_are_off_by_default_and_each_switches_one_thing()
{
    State s;
    s.set(SIG_ENG_TEMP, 90, 1);
    s.set(SIG_FUEL_LEVEL, 31, 1);
    s.set(SIG_ODO_CLUSTER, 123456, 1);
    s.set(SIG_CONS_INST, 72, 1);
    s.set(SIG_CONS_AVG, 65, 1);
    s.set(SIG_RANGE_CALC, 477, 1);
    E e;
    e.begin(expCapture, nullptr);
    TEST_ASSERT_EQUAL_UINT8(0, e.experiments());
    auto run = [&](uint8_t exp) {
        g_sub09 = g_cmd21 = g_cmd22 = g_cmd23 = 0;
        memset(g_odo, 0, sizeof(g_odo));
        e.setExperiments(exp);
        for (uint32_t t = 0; t < 12000; t += 100) e.tick(s, 100000 * (exp + 1) + t);
    };
    run(0);                                         // nothing new reaches the head unit
    TEST_ASSERT_EQUAL_INT(0, g_sub09);
    TEST_ASSERT_EQUAL_INT(0, g_cmd21 + g_cmd22 + g_cmd23);
    TEST_ASSERT_EQUAL_UINT8(0, g_odo[1]);           // no 0x5FD odometer: no 7D 04
    run(E::EXP_COOLANT);
    TEST_ASSERT_TRUE(g_sub09 > 0);
    TEST_ASSERT_EQUAL_INT(0, g_cmd22);
    run(E::EXP_FUEL_7D04);                          // the cluster's odometer, 31.0 L in D4-D5
    TEST_ASSERT_EQUAL_INT(0, g_sub09);
    const uint8_t odo[] = {0x2E, 0x7D, 0x0C, 0x04, 0x40, 0xE2, 0x01, 0x36, 0x01};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(odo, g_odo, sizeof(odo));
    run(E::EXP_COMPUTED);
    TEST_ASSERT_TRUE(g_cmd21 > 0 && g_cmd22 > 0 && g_cmd23 > 0);
    // A bound Raise signal still wins over the computed one.
    uint8_t frame[64], len = 0;
    TEST_ASSERT_TRUE(e.preview(E::MSG_FUEL_INST, s, frame, len));
    TEST_ASSERT_EQUAL_HEX8(72, frame[5]);
    s.pin(SIG_FUEL_INST, 99);
    TEST_ASSERT_TRUE(e.preview(E::MSG_FUEL_INST, s, frame, len));
    TEST_ASSERT_EQUAL_HEX8(99, frame[5]);
    // The coolant frame itself: 0x7D 09 [00][78 - 90 = 0xF4]; the tablet
    // shows (78 - D2) mod 256 (owner's sweep 2026-10-25), so 90.
    TEST_ASSERT_TRUE(e.preview(E::MSG_COOLANT, s, frame, len));
    const uint8_t cool[] = {0x2E, 0x7D, 0x03, 0x09, 0x00, 0xF4};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cool, frame, sizeof(cool));
}

static int g_sub06;
static uint8_t g_gear[8];
static void gearCapture(const uint8_t* f, uint8_t n, void*)
{
    if (f[1] == 0x7D && f[3] == 0x06) { ++g_sub06; if (n <= sizeof(g_gear)) memcpy(g_gear, f, n); }
}

// 0x7D 06 is an unknown frame to the tablet: nothing builds it, schedule or
// test, until the selector experiment is on, and then only from the probe.
void test_selector_probe_goes_out_only_with_its_experiment()
{
    State s;
    E e;
    e.begin(gearCapture, nullptr);
    uint8_t frame[64], len = 0;
    g_sub06 = 0;
    s.pin(SIG_PRND, 2);
    for (uint32_t t = 0; t < 5000; t += 100) e.tick(s, 1000 + t);
    TEST_ASSERT_EQUAL_INT(0, g_sub06);
    TEST_ASSERT_FALSE(e.preview(E::MSG_GEAR, s, frame, len));
    TEST_ASSERT_FALSE(e.sendNow(E::MSG_GEAR, s));
    // On: 0x7D 06 [00][02], and each new value goes out at once.
    e.setExperiments(E::EXP_GEAR);
    for (uint32_t t = 0; t < 2000; t += 100) e.tick(s, 10000 + t);
    TEST_ASSERT_TRUE(g_sub06 > 0);
    const uint8_t two[] = {0x2E, 0x7D, 0x03, 0x06, 0x00, 0x02};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(two, g_gear, sizeof(two));
    s.pin(SIG_PRND, 9);
    e.tick(s, 12050);
    TEST_ASSERT_EQUAL_HEX8(0x09, g_gear[5]);
    // The probe is clamped to the nibble by its signal's range.
    s.pin(SIG_PRND, 40);
    TEST_ASSERT_TRUE(e.preview(E::MSG_GEAR, s, frame, len));
    TEST_ASSERT_EQUAL_HEX8(0x0F, frame[5]);
    // No probe value: nothing, even with the switch on.
    s.unpin(SIG_PRND);
    TEST_ASSERT_FALSE(s.has(SIG_PRND));
    TEST_ASSERT_FALSE(e.preview(E::MSG_GEAR, s, frame, len));
}

// Battery volts in a borrowed field (the RAV4 profile has none): volts x10,
// 12.6 V -> 126, in the one field chosen; never beside the experiment that
// fills the same field.
void test_battery_in_a_borrowed_field()
{
    TEST_ASSERT_EQUAL_UINT16(126, E::battField(1260));
    TEST_ASSERT_EQUAL_UINT16(126, E::battField(1255));               // rounded
    TEST_ASSERT_EQUAL_UINT16(0, E::battField(-5));
    State s;
    s.set(SIG_BATT_V, 1260, 1);                                      // 12.60 V
    s.set(SIG_ODO_CLUSTER, 1000, 1);
    E e;
    uint8_t f[64], n = 0;
    // 0x21: the range, BE at D4-D5.
    e.setExperiments(E::EXP_BATT | (uint8_t)((uint8_t)E::BattAt::Range21 << 5));
    TEST_ASSERT_TRUE(e.preview(E::MSG_TRIP, s, f, n));
    TEST_ASSERT_EQUAL_HEX8(0x21, f[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, f[3 + 4]);
    TEST_ASSERT_EQUAL_HEX8(126, f[3 + 5]);
    // 0x7D 04 D4-D5, LE; the cluster's odometer carries the frame.
    e.setExperiments(E::withBattAt(E::EXP_BATT, E::BattAt::Odo7D04));
    TEST_ASSERT_TRUE(e.preview(E::MSG_ODO, s, f, n));
    TEST_ASSERT_EQUAL_HEX8(0x04, f[3]);
    TEST_ASSERT_EQUAL_HEX8(126, f[3 + 4]);
    TEST_ASSERT_EQUAL_HEX8(0x00, f[3 + 5]);
    // 0x23: the current value, BE at D1-D2.
    e.setExperiments(E::withBattAt(E::EXP_BATT, E::BattAt::Hist23));
    TEST_ASSERT_TRUE(e.preview(E::MSG_FUEL_HIST, s, f, n));
    TEST_ASSERT_EQUAL_HEX8(0x23, f[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, f[3 + 1]);
    TEST_ASSERT_EQUAL_HEX8(126, f[3 + 2]);
    // Off: 0x23 has nothing to say, 0x7D 04 keeps F2 08.
    e.setExperiments(0);
    TEST_ASSERT_FALSE(e.preview(E::MSG_FUEL_HIST, s, f, n));
}

void test_battery_and_its_rival_are_never_both_on()
{
    const uint8_t at21 = E::withBattAt(E::EXP_BATT, E::BattAt::Range21);
    const uint8_t at04 = E::withBattAt(E::EXP_BATT, E::BattAt::Odo7D04);
    const uint8_t at23 = E::withBattAt(E::EXP_BATT, E::BattAt::Hist23);
    // The web refuses a clash...
    TEST_ASSERT_NOT_NULL(E::experimentsWhy(at21 | E::EXP_COMPUTED));
    TEST_ASSERT_NOT_NULL(E::experimentsWhy(at23 | E::EXP_COMPUTED));
    TEST_ASSERT_NOT_NULL(E::experimentsWhy(at04 | E::EXP_FUEL_7D04));
    TEST_ASSERT_NOT_NULL(E::experimentsWhy((uint8_t)(E::EXP_BATT | (3 << 5))));   // no field 3
    TEST_ASSERT_NOT_NULL(E::experimentsWhy(0x80));
    // ...and takes the rest, the rival of another field included.
    TEST_ASSERT_NULL(E::experimentsWhy(at21 | E::EXP_FUEL_7D04 | E::EXP_COOLANT | E::EXP_GEAR));
    TEST_ASSERT_NULL(E::experimentsWhy(at04 | E::EXP_COMPUTED));
    TEST_ASSERT_NULL(E::experimentsWhy(E::EXP_COMPUTED | E::EXP_FUEL_7D04));
    // The emitter settles one that got past (an old NVS byte) for the battery.
    E e;
    e.setExperiments(at21 | E::EXP_COMPUTED | E::EXP_FUEL_7D04);
    TEST_ASSERT_EQUAL_HEX8(at21 | E::EXP_FUEL_7D04, e.experiments());
    e.setExperiments(at04 | E::EXP_FUEL_7D04 | E::EXP_COMPUTED);
    TEST_ASSERT_EQUAL_HEX8(at04 | E::EXP_COMPUTED, e.experiments());
    e.setExperiments((uint8_t)(E::EXP_BATT | (3 << 5)));
    TEST_ASSERT_EQUAL_HEX8(at21, e.experiments());
    e.setExperiments(0xFF);
    TEST_ASSERT_EQUAL_HEX8(0, e.experiments() & 0x80);
    // Without the battery the old switches are as they were.
    e.setExperiments(E::EXP_COMPUTED | E::EXP_FUEL_7D04);
    TEST_ASSERT_EQUAL_HEX8(E::EXP_COMPUTED | E::EXP_FUEL_7D04, e.experiments());
}

// The climate overlay pops on a changed 0x28. The outside temperature in it
// is held: flicker never changes the frame, a lasting change does after the
// hold, a test pin at once.
void test_outside_temp_is_held_against_flicker()
{
    Canbox::TempHold h;
    TEST_ASSERT_EQUAL_INT32(12, h.step(12, 0));          // the first reading at once
    TEST_ASSERT_EQUAL_INT32(12, h.step(13, 1000));       // flicker 12/13 ...
    TEST_ASSERT_EQUAL_INT32(12, h.step(12, 2000));
    TEST_ASSERT_EQUAL_INT32(12, h.step(13, 3000));       // ... the hold restarts
    TEST_ASSERT_EQUAL_INT32(12, h.step(13, 3000 + Canbox::TempHold::HOLD_MS - 1));
    TEST_ASSERT_EQUAL_INT32(13, h.step(13, 3000 + Canbox::TempHold::HOLD_MS));
    TEST_ASSERT_EQUAL_INT32(13, h.step(13, 200000));
}

static int g_frames28;
static uint8_t g_d5[8];
static void cap28(const uint8_t* f, uint8_t n, void*)
{
    if (f[1] == 0x28) { if (g_frames28 < 8 && n > 8) g_d5[g_frames28] = f[3 + 5]; ++g_frames28; }
}

void test_climate_frame_changes_only_on_a_lasting_value()
{
    State s;
    E e;
    e.begin(cap28, nullptr);
    g_frames28 = 0;
    uint32_t t = 0;
    // 30 minutes of a reading flickering between 12 and 13 C every 2 s.
    for (int i = 0; i < 900; ++i) {
        t += 2000;
        s.set(SIG_OUT_TEMP, (i & 1) ? 13 : 12, t);
        e.tick(s, t);
    }
    TEST_ASSERT_TRUE(g_frames28 > 0);
    TEST_ASSERT_EQUAL_UINT32(0, e.climateChanges());     // one value the whole time
    // A real change, held for a minute: one change.
    for (int i = 0; i < 40; ++i) { t += 2000; s.set(SIG_OUT_TEMP, 15, t); e.tick(s, t); }
    TEST_ASSERT_EQUAL_UINT32(1, e.climateChanges());
    // A pinned test value goes at once.
    s.pin(SIG_OUT_TEMP, 20);
    uint8_t f[64], n = 0;
    TEST_ASSERT_TRUE(e.preview(E::MSG_TEMP, s, f, n));
    TEST_ASSERT_EQUAL_HEX8((20 + 40) * 2, f[3 + 5]);
}

// The A/C form never goes out on guessed rows: only a measured signal or a
// hand-pinned one may open the overlay.
void test_ac_form_needs_a_trusted_signal()
{
    State s;
    E e;
    e.setCmd28(E::Cmd28::AirCon);
    uint8_t f[64], n = 0;
    s.set(SIG_AC_ON, 1, 10);                                     // decoded, not measured
    Vehicle::setVerified(SIG_AC_ON, false);
    TEST_ASSERT_FALSE(E::acTrusted(s));
    TEST_ASSERT_FALSE(e.preview(E::MSG_AC, s, f, n));
    Vehicle::setVerified(SIG_AC_ON, true);                       // measured in the car
    TEST_ASSERT_TRUE(e.preview(E::MSG_AC, s, f, n));
    TEST_ASSERT_EQUAL_HEX8(0x80, f[3]);
    Vehicle::setVerified(SIG_AC_ON, false);
    State p;
    p.pin(SIG_AC_FAN, 3);                                        // a test or a constant
    TEST_ASSERT_TRUE(E::acTrusted(p));
    TEST_ASSERT_TRUE(e.preview(E::MSG_AC, p, f, n));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_experiments_are_off_by_default_and_each_switches_one_thing);
    RUN_TEST(test_battery_in_a_borrowed_field);
    RUN_TEST(test_battery_and_its_rival_are_never_both_on);
    RUN_TEST(test_outside_temp_is_held_against_flicker);
    RUN_TEST(test_climate_frame_changes_only_on_a_lasting_value);
    RUN_TEST(test_ac_form_needs_a_trusted_signal);
    RUN_TEST(test_selector_probe_goes_out_only_with_its_experiment);
    RUN_TEST(test_raise_targets_ids_are_unique_and_name_real_signals);
    RUN_TEST(test_every_sent_raise_target_changes_its_message);
    RUN_TEST(test_raise_values_place_the_decimal_point);
    RUN_TEST(test_an_old_doors_length_one_is_migrated_once);
    RUN_TEST(test_the_migration_marker_reads_as_rpm_times_four_in_the_old_layout);
    RUN_TEST(test_pin_beats_can_and_never_goes_stale);
    RUN_TEST(test_pin_makes_an_unknown_signal_sendable);
    RUN_TEST(test_pin_is_clamped_to_the_signal_range);
    RUN_TEST(test_every_signal_has_metadata);

    RUN_TEST(test_silent_about_data_it_does_not_have);
    RUN_TEST(test_pinned_temperature_is_emitted_with_the_documented_encoding);
    RUN_TEST(test_send_now_refuses_a_message_with_no_data);
    RUN_TEST(test_on_change_message_beats_its_interval);
    RUN_TEST(test_on_change_sees_the_fifth_payload_byte);
    RUN_TEST(test_disabled_emitter_is_silent_but_send_now_still_works);
    RUN_TEST(test_speed_is_little_endian_hundredths);

    RUN_TEST(test_rpm_goes_on_the_wire_times_four);
    RUN_TEST(test_doors_are_keyed_by_side_not_by_role);
    RUN_TEST(test_doors_defaults_to_length_two);
    RUN_TEST(test_doors_message_carries_the_bonnet);
    RUN_TEST(test_doors_message_carries_the_ignition_bit);
    RUN_TEST(test_doors_length_is_switchable);
    RUN_TEST(test_speed_carries_average_in_the_trailing_pair);
    RUN_TEST(test_steering_is_signed_int16_lsb_first);
    RUN_TEST(test_steering_clamps_and_inverts);
    RUN_TEST(test_trip_is_big_endian_and_history_is_thirteen_bytes);
    RUN_TEST(test_instant_fuel_matches_the_worked_example);

    RUN_TEST(test_the_two_0x28_forms_are_mutually_exclusive);
    RUN_TEST(test_aircon_packs_the_documented_bits);
    RUN_TEST(test_radar_front_is_mirrored_relative_to_rear);
    RUN_TEST(test_rear_radar_is_scheduled_before_front);
    RUN_TEST(test_tpms_unknown_wheels_are_no_data_not_zero);
    RUN_TEST(test_hybrid_stays_silent_until_explicitly_asked_for);
    RUN_TEST(test_lights_match_the_worked_example);
    RUN_TEST(test_odometer_matches_the_worked_example);

    RUN_TEST(test_send_by_cmd_answers_a_request);
    RUN_TEST(test_send_by_cmd_honours_the_selected_0x28_form);
    RUN_TEST(test_a_disabled_message_leaves_the_schedule_but_not_the_button);
    RUN_TEST(test_preview_matches_what_is_sent);
    RUN_TEST(test_every_message_has_a_name_and_a_command);

    RUN_TEST(test_tuning_survives_a_blob_round_trip);
    RUN_TEST(test_a_blob_from_another_layout_is_rejected);
    RUN_TEST(test_an_absurd_history_length_falls_back_to_the_spec_value);
    RUN_TEST(test_the_steering_knobs_each_change_the_wire);
    RUN_TEST(test_the_unit_bytes_are_settable);
    RUN_TEST(test_trip_fields_can_be_blanked);
    RUN_TEST(test_radar_polarity_inverts_but_leaves_no_display_alone);
    RUN_TEST(test_the_version_string_is_settable_and_always_sixteen_bytes);
    return UNITY_END();
}
