#include <unity.h>
#include <string.h>
#include "keys/AffaKeySource.h"
#include "keys/AffaMap.h"
#include "keys/KeyEngine.h"
#include "proto/canbox/Rav4Profile.h"

using namespace Keys;
using affa::Key;
using affa::KeyEdge;

void setUp() {}
void tearDown() {}

// A sink that just records what the engine dispatched.
struct RecSink : public ISink {
    static constexpr int MAX = 16;
    Event ev[MAX];
    int   n = 0;
    void onKey(const Event& e) override { if (n < MAX) ev[n++] = e; }
    const char* id() const override { return "rec"; }
};

// The whole rig: a default map, a source bound to it, and a recording sink.
struct Rig {
    AffaMap       map;
    KeyEngine     engine;
    RecSink       sink;
    AffaKeySource src;

    Rig() {
        map.loadDefaults();
        engine.addSink(&sink);
        src.begin(&engine, &map);
    }
    void press(Key k, bool hold = false) {
        AffaKeySource::handle(k, hold ? KeyEdge::Hold : KeyEdge::Click, &src);
    }
};

// ---- the map ----------------------------------------------------------------
void test_defaults_cover_every_click_and_the_wheel()
{
    AffaMap m;
    m.loadDefaults();

    AffaMap::Row r;
    TEST_ASSERT_TRUE(m.lookup(Key::VolUp,    false, r)); TEST_ASSERT_TRUE(r.action == Action::VolUp);
    TEST_ASSERT_TRUE(m.lookup(Key::VolDown,  false, r)); TEST_ASSERT_TRUE(r.action == Action::VolDown);
    TEST_ASSERT_TRUE(m.lookup(Key::SrcNext,  false, r)); TEST_ASSERT_TRUE(r.action == Action::SrcNext);
    TEST_ASSERT_TRUE(m.lookup(Key::SrcPrev,  false, r)); TEST_ASSERT_TRUE(r.action == Action::SrcPrev);
    TEST_ASSERT_TRUE(m.lookup(Key::Load,     false, r)); TEST_ASSERT_TRUE(r.action == Action::Load);
    TEST_ASSERT_TRUE(m.lookup(Key::Pause,    false, r)); TEST_ASSERT_TRUE(r.action == Action::Ok);
    TEST_ASSERT_TRUE(m.lookup(Key::RollUp,   false, r)); TEST_ASSERT_TRUE(r.action == Action::EncCw);
    TEST_ASSERT_TRUE(m.lookup(Key::RollDown, false, r)); TEST_ASSERT_TRUE(r.action == Action::EncCcw);
}

// Hold-SRC is MUTE on the OEM stalk, and it is a PLAIN press of the mute
// mapping rather than a long press of SRC — the two produce different frames.
void test_hold_source_is_mute_by_default()
{
    AffaMap m;
    m.loadDefaults();
    AffaMap::Row r;
    TEST_ASSERT_TRUE(m.lookup(Key::SrcNext, true, r));
    TEST_ASSERT_TRUE(r.action == Action::Mute);
    TEST_ASSERT_FALSE(r.asLong);
}

// A held wheel detent is indistinguishable on the wire from a RollUp hold
// (0x0101|0xC0 and 0x0141|0xC0 are both 0x01C1), so acting on it would step the
// wrong way about half the time. Empty by default, and that is a decision.
void test_the_wheel_hold_rows_are_empty_by_default()
{
    AffaMap m;
    m.loadDefaults();
    AffaMap::Row r;
    TEST_ASSERT_FALSE(m.lookup(Key::RollUp,   true, r));
    TEST_ASSERT_FALSE(m.lookup(Key::RollDown, true, r));
}

void test_an_unknown_wire_code_has_no_row()
{
    AffaMap m;
    m.loadDefaults();
    AffaMap::Row r;
    TEST_ASSERT_FALSE(m.lookup(static_cast<Key>(0x0999), false, r));
    TEST_ASSERT_EQUAL_UINT8(AffaMap::KEYS, AffaMap::indexOf(static_cast<Key>(0x0999)));
}

void test_the_map_survives_a_round_trip_through_nvs()
{
    AffaMap a;
    a.loadDefaults();
    AffaMap::Row r; r.action = Action::Radio; r.code = 0x88; r.asLong = true;
    a.setRow(AffaMap::rowIndex(0, true), r);

    uint8_t blob[AffaMap::BLOB_SIZE];
    a.toBlob(blob);

    AffaMap b;
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));
    const AffaMap::Row& got = b.row(AffaMap::rowIndex(0, true));
    TEST_ASSERT_TRUE(got.action == Action::Radio);
    TEST_ASSERT_EQUAL_UINT8(0x88, got.code);
    TEST_ASSERT_TRUE(got.asLong);
}

// A blob written by a firmware that knew more actions than this one must be
// rejected whole. Half a mapping is much harder to notice than one that
// visibly reverted to defaults.
void test_a_blob_with_an_out_of_range_action_is_rejected_whole()
{
    AffaMap a;
    a.loadDefaults();
    uint8_t blob[AffaMap::BLOB_SIZE];
    a.toBlob(blob);
    blob[3] = 0xEE;                       // an action this build does not have

    AffaMap b;
    b.loadDefaults();
    TEST_ASSERT_FALSE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_FALSE(b.fromBlob(blob, sizeof(blob) - 1));   // wrong length too
}

// ---- the source -------------------------------------------------------------
void test_a_click_becomes_a_press_release_pair()
{
    Rig g;
    // AFFA delivers a click with no release edge, so the pair is synthesised —
    // otherwise every head-unit sink would be left holding a key down for ever.
    g.press(Key::VolUp);
    TEST_ASSERT_EQUAL_INT(2, g.sink.n);
    TEST_ASSERT_TRUE(g.sink.ev[0].state == State::Press);
    TEST_ASSERT_TRUE(g.sink.ev[1].state == State::Release);
    TEST_ASSERT_TRUE(g.sink.ev[0].action == Action::VolUp);
    TEST_ASSERT_EQUAL_UINT8(3, g.sink.ev[0].srcId);   // 3 = display over CAN
}

void test_hold_arrives_as_a_long_press_when_the_row_says_so()
{
    Rig g;
    g.press(Key::VolUp, true);
    TEST_ASSERT_EQUAL_INT(2, g.sink.n);
    TEST_ASSERT_TRUE(g.sink.ev[0].state == State::Long);
    TEST_ASSERT_TRUE(g.sink.ev[1].state == State::Release);
}

// asLong is about what the HEAD UNIT is told, not about what the panel said.
// Remapping hold-SRC to MUTE has to produce a plain press of the mute mapping.
void test_a_remapped_hold_is_a_plain_press_of_the_new_meaning()
{
    Rig g;
    g.press(Key::SrcNext, true);
    TEST_ASSERT_EQUAL_INT(2, g.sink.n);
    TEST_ASSERT_TRUE(g.sink.ev[0].action == Action::Mute);
    TEST_ASSERT_TRUE(g.sink.ev[0].state == State::Press);
}

void test_wheel_detent_is_a_single_event()
{
    Rig g;
    // The wheel has no release to synthesise, and the canbox sink turns this
    // into a detent count — a second event here would be a phantom click.
    g.press(Key::RollUp);
    TEST_ASSERT_EQUAL_INT(1, g.sink.n);
    TEST_ASSERT_TRUE(g.sink.ev[0].action == Action::EncCw);
    TEST_ASSERT_TRUE(g.sink.ev[0].state == State::Press);
}

void test_a_wheel_hold_produces_nothing_and_is_counted()
{
    Rig g;
    g.press(Key::RollUp, true);
    TEST_ASSERT_EQUAL_INT(0, g.sink.n);
    TEST_ASSERT_EQUAL_UINT32(1, g.src.unmapped());
}

// The enum is open: an unrecognised wire code arrives carrying its raw value
// rather than being dropped by the library. It must be COUNTED and NAMED — a
// stalk we have not seen is something to find out about.
void test_unknown_key_is_counted_and_not_forwarded()
{
    Rig g;
    g.press(static_cast<Key>(0x0999));
    TEST_ASSERT_EQUAL_INT(0, g.sink.n);
    TEST_ASSERT_EQUAL_UINT32(1, g.src.unmapped());
    TEST_ASSERT_EQUAL_UINT16(0x0999, g.src.lastUnmapped());
    TEST_ASSERT_FALSE(g.src.lastUnmappedHold());
    TEST_ASSERT_EQUAL_UINT32(0, g.src.events());
}

// The whole point of the table: remapping from the web UI changes what the head
// unit is told, with no reflash and without touching any code.
void test_a_remapped_row_changes_what_reaches_the_sink()
{
    Rig g;
    AffaMap::Row r;
    r.action = Action::Voice;             // Pause click now means "voice"
    r.code   = 0;                         // profile default for Voice
    r.asLong = false;
    g.map.setRow(AffaMap::rowIndex(5, false), r);

    g.press(Key::Pause);
    TEST_ASSERT_EQUAL_INT(2, g.sink.n);
    TEST_ASSERT_TRUE(g.sink.ev[0].action == Action::Voice);
    TEST_ASSERT_EQUAL_UINT8(0, g.sink.ev[0].codeHint);
}

// The literal-code escape hatch: the RAV4 table has buttons our logical
// vocabulary has no name for, and walking codes is how a dead button gets
// diagnosed. A non-zero code must travel as codeHint and beat the default.
void test_a_literal_code_override_travels_as_a_code_hint()
{
    Rig g;
    AffaMap::Row r;
    r.action = Action::Ok;
    r.code   = Canbox::RAV4_BACK;         // 0x15, which no action maps to
    g.map.setRow(AffaMap::rowIndex(5, false), r);

    g.press(Key::Pause);
    TEST_ASSERT_EQUAL_INT(2, g.sink.n);
    TEST_ASSERT_EQUAL_UINT8(Canbox::RAV4_BACK, g.sink.ev[0].codeHint);
    TEST_ASSERT_EQUAL_UINT8(Canbox::RAV4_BACK, g.sink.ev[1].codeHint);   // and the release
}

void test_an_emptied_row_silences_that_button()
{
    Rig g;
    g.map.setRow(AffaMap::rowIndex(3, false), AffaMap::Row{});   // VolUp click
    g.press(Key::VolUp);
    TEST_ASSERT_EQUAL_INT(0, g.sink.n);
    TEST_ASSERT_EQUAL_UINT32(1, g.src.unmapped());
}

void test_every_key_has_a_name_and_round_trips_through_its_index()
{
    for (uint8_t i = 0; i < AffaMap::KEYS; ++i) {
        TEST_ASSERT_NOT_NULL(AffaMap::keyName(i));
        TEST_ASSERT_TRUE(AffaMap::keyName(i)[0] != '?');
        TEST_ASSERT_EQUAL_UINT8(i, AffaMap::indexOf(AffaMap::keyAt(i)));
    }
}

// ---- the key as plain bytes (Step 2) ------------------------------------------
// The panel's callback posts the wire code and the hold flag as integers; the
// keys task maps them here. The source time travels with them, so the sink
// schedules a release from when the key was decoded, not when it was mapped.

void test_a_plain_code_maps_exactly_like_the_library_key()
{
    Rig a, b;
    a.press(Key::VolUp);
    b.src.onPanelKey((uint16_t)Key::VolUp, false, 0);
    TEST_ASSERT_EQUAL(a.sink.n, b.sink.n);
    for (int i = 0; i < a.sink.n; ++i) {
        TEST_ASSERT_TRUE(a.sink.ev[i].action == b.sink.ev[i].action);
        TEST_ASSERT_TRUE(a.sink.ev[i].state  == b.sink.ev[i].state);
        TEST_ASSERT_EQUAL(a.sink.ev[i].codeHint, b.sink.ev[i].codeHint);
    }
}

void test_a_plain_code_carries_its_source_time()
{
    Rig r;
    r.src.onPanelKey((uint16_t)Key::RollUp, false, 4242);
    TEST_ASSERT_EQUAL(1, r.sink.n);
    TEST_ASSERT_EQUAL(4242u, r.sink.ev[0].tMs);
}

void test_a_plain_hold_flag_selects_the_hold_row()
{
    Rig r;
    r.src.onPanelKey((uint16_t)Key::SrcNext, true, 10);
    TEST_ASSERT_EQUAL(2, r.sink.n);
    TEST_ASSERT_TRUE(r.sink.ev[0].action == Action::Mute);
}

void test_an_unknown_plain_code_is_counted_with_its_raw_value()
{
    Rig r;
    r.src.onPanelKey(0x0113, false, 10);
    TEST_ASSERT_EQUAL(0, r.sink.n);
    TEST_ASSERT_EQUAL(1u, r.src.unmapped());
    TEST_ASSERT_EQUAL_HEX16(0x0113, r.src.lastUnmapped());
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_cover_every_click_and_the_wheel);
    RUN_TEST(test_hold_source_is_mute_by_default);
    RUN_TEST(test_the_wheel_hold_rows_are_empty_by_default);
    RUN_TEST(test_an_unknown_wire_code_has_no_row);
    RUN_TEST(test_the_map_survives_a_round_trip_through_nvs);
    RUN_TEST(test_a_blob_with_an_out_of_range_action_is_rejected_whole);

    RUN_TEST(test_a_click_becomes_a_press_release_pair);
    RUN_TEST(test_hold_arrives_as_a_long_press_when_the_row_says_so);
    RUN_TEST(test_a_remapped_hold_is_a_plain_press_of_the_new_meaning);
    RUN_TEST(test_wheel_detent_is_a_single_event);
    RUN_TEST(test_a_wheel_hold_produces_nothing_and_is_counted);
    RUN_TEST(test_unknown_key_is_counted_and_not_forwarded);
    RUN_TEST(test_a_remapped_row_changes_what_reaches_the_sink);
    RUN_TEST(test_a_literal_code_override_travels_as_a_code_hint);
    RUN_TEST(test_an_emptied_row_silences_that_button);
    RUN_TEST(test_every_key_has_a_name_and_round_trips_through_its_index);
    RUN_TEST(test_a_plain_code_maps_exactly_like_the_library_key);
    RUN_TEST(test_a_plain_code_carries_its_source_time);
    RUN_TEST(test_a_plain_hold_flag_selects_the_hold_row);
    RUN_TEST(test_an_unknown_plain_code_is_counted_with_its_raw_value);
    return UNITY_END();
}
