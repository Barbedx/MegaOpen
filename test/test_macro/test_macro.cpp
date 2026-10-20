// Combinations, and the two ways they can be dangerous.
//
// A macro is pattern matching over a stream of timed events, and every one of
// its failure modes is invisible from a driver's seat:
//
//   * a SWALLOWED press that never comes back. The engine holds taps in order to
//     tell "MUTE, MUTE, MUTE" from three mutes, and if the window lapses without
//     the macro completing it has to give every one of them back, in order. A
//     dropped press reads as "the button works most of the time", which is the
//     most expensive bug this firmware could ship.
//   * a KEY LEFT DOWN at the head unit. A combo consumes the press that
//     completes it, but the members held before it have already been dispatched
//     — so each one needs a synthetic release, or the unit holds VOL+ for ever
//     and the volume runs to maximum on its own.
//
// Both are choreography over time, and a multimeter or a dashboard shows one
// instant. That is what these tests are for.
#include <unity.h>
#include <string.h>

#include "keys/MacroEngine.h"

using namespace Keys;

void setUp() {}
void tearDown() {}

// ---- harness ---------------------------------------------------------------

static Event    g_emitted[32];
static uint8_t  g_emitN = 0;
static Macro    g_fired[8];
static uint8_t  g_fireN = 0;

static void onEmit(const Event& e, void*) { if (g_emitN < 32) g_emitted[g_emitN++] = e; }
static void onFire(const Macro& m, void*) { if (g_fireN < 8)  g_fired[g_fireN++] = m; }

// What actually reached the sinks, in order: the events the engine let through
// plus the ones it emitted itself. This is the only thing the head unit sees, so
// it is the only thing worth asserting on.
static Event   g_out[64];
static uint8_t g_outN = 0;

static void feed(MacroEngine& m, Action a, State s, uint32_t t)
{
    g_emitN = 0;
    Event e; e.action = a; e.state = s; e.tMs = t;
    const bool consumed = m.observe(e);
    for (uint8_t i = 0; i < g_emitN; ++i) if (g_outN < 64) g_out[g_outN++] = g_emitted[i];
    if (!consumed && g_outN < 64) g_out[g_outN++] = e;
}

static void reset(MacroEngine& m)
{
    g_emitN = g_fireN = g_outN = 0;
    m.clear();
    m.onEmit(onEmit, nullptr);
    m.onFire(onFire, nullptr);
}

static void assertOut(uint8_t i, Action a, State s)
{
    TEST_ASSERT_TRUE_MESSAGE(i < g_outN, "fewer events reached the sinks than expected");
    TEST_ASSERT_EQUAL_STRING(name(a), name(g_out[i].action));
    TEST_ASSERT_EQUAL((int)s, (int)g_out[i].state);
}

// ---- the plain case: nothing configured changes nothing --------------------

static void test_an_empty_table_passes_everything_through()
{
    MacroEngine m; reset(m);
    feed(m, Action::VolUp, State::Press,   0);
    feed(m, Action::VolUp, State::Release, 40);
    TEST_ASSERT_EQUAL(2, g_outN);
    TEST_ASSERT_EQUAL(0, m.fired());
}

static void test_a_disabled_macro_is_not_a_macro()
{
    MacroEngine m; reset(m);
    m.at(0).enabled = false;
    m.at(0).trigger = Trigger::Combo;
    m.at(0).keys[0] = Action::VolUp; m.at(0).keys[1] = Action::VolDown;
    m.at(0).members = 2;
    m.at(0).effect  = Effect::KeyCode; m.at(0).code = 0x07;

    feed(m, Action::VolUp,   State::Press, 0);
    feed(m, Action::VolDown, State::Press, 20);
    TEST_ASSERT_EQUAL(0, m.fired());
    TEST_ASSERT_EQUAL(2, g_outN);
}

// ---- combos ----------------------------------------------------------------

static void armCombo(MacroEngine& m, bool swallow)
{
    Macro& a = m.at(0);
    a.enabled = true;
    a.trigger = Trigger::Combo;
    a.keys[0] = Action::VolUp; a.keys[1] = Action::VolDown; a.members = 2;
    a.effect  = Effect::KeyCode; a.code = 0x07;
    a.swallow = swallow;
}

static void test_a_combo_fires_when_the_second_member_lands()
{
    MacroEngine m; reset(m); armCombo(m, true);

    feed(m, Action::VolUp, State::Press, 0);
    TEST_ASSERT_EQUAL(0, m.fired());          // one key is not a combination
    feed(m, Action::VolDown, State::Press, 30);
    TEST_ASSERT_EQUAL(1, m.fired());
    TEST_ASSERT_EQUAL(1, g_fireN);
    TEST_ASSERT_EQUAL(0x07, g_fired[0].code);
}

// The headline: the first press is already gone — keys are never delayed — but
// nothing may be left HELD when the macro fires.
static void test_a_swallowed_combo_releases_the_member_it_already_sent()
{
    MacroEngine m; reset(m); armCombo(m, true);

    feed(m, Action::VolUp,   State::Press, 0);
    feed(m, Action::VolDown, State::Press, 30);

    // VOL+ press went out (unavoidable), then its synthetic release. VOL-'s
    // press was consumed and must NOT appear at all.
    TEST_ASSERT_EQUAL(2, g_outN);
    assertOut(0, Action::VolUp, State::Press);
    assertOut(1, Action::VolUp, State::Release);
}

static void test_the_real_releases_of_a_swallowed_combo_never_reach_a_sink()
{
    MacroEngine m; reset(m); armCombo(m, true);

    feed(m, Action::VolUp,   State::Press,   0);
    feed(m, Action::VolDown, State::Press,   30);
    feed(m, Action::VolUp,   State::Release, 200);
    feed(m, Action::VolDown, State::Release, 210);

    // A second VOL+ release would be a button coming up twice; VOL-'s release
    // would be a button coming up that never went down.
    TEST_ASSERT_EQUAL(2, g_outN);
}

static void test_a_combo_re_arms_after_the_fingers_come_off()
{
    MacroEngine m; reset(m); armCombo(m, true);

    for (uint32_t round = 0; round < 3; ++round) {
        const uint32_t t = round * 1000;
        feed(m, Action::VolUp,   State::Press,   t);
        feed(m, Action::VolDown, State::Press,   t + 30);
        feed(m, Action::VolUp,   State::Release, t + 200);
        feed(m, Action::VolDown, State::Release, t + 210);
    }
    // Three rounds, three fires. This is the test that catches a re-arm living
    // where the swallowed release can never reach it.
    TEST_ASSERT_EQUAL(3, m.fired());
}

static void test_a_combo_does_not_re_fire_while_it_is_still_held()
{
    MacroEngine m; reset(m); armCombo(m, true);

    feed(m, Action::VolUp,   State::Press, 0);
    feed(m, Action::VolDown, State::Press, 30);
    feed(m, Action::Mute,    State::Press, 60);    // unrelated key, combo still held
    feed(m, Action::Mute,    State::Release, 90);
    TEST_ASSERT_EQUAL(1, m.fired());
}

static void test_a_combo_that_does_not_swallow_lets_both_keys_through()
{
    MacroEngine m; reset(m); armCombo(m, false);

    feed(m, Action::VolUp,   State::Press,   0);
    feed(m, Action::VolDown, State::Press,   30);
    feed(m, Action::VolUp,   State::Release, 200);
    feed(m, Action::VolDown, State::Release, 210);

    TEST_ASSERT_EQUAL(1, m.fired());
    TEST_ASSERT_EQUAL(4, g_outN);      // every event, untouched
}

static void test_a_partial_combo_is_just_two_ordinary_keys()
{
    MacroEngine m; reset(m); armCombo(m, true);

    feed(m, Action::VolUp,   State::Press,   0);
    feed(m, Action::VolUp,   State::Release, 100);
    feed(m, Action::VolDown, State::Press,   200);
    feed(m, Action::VolDown, State::Release, 300);

    TEST_ASSERT_EQUAL(0, m.fired());
    TEST_ASSERT_EQUAL(4, g_outN);
}

static void test_a_three_key_combo_needs_all_three()
{
    MacroEngine m; reset(m);
    Macro& a = m.at(0);
    a.enabled = true; a.trigger = Trigger::Combo;
    a.keys[0] = Action::VolUp; a.keys[1] = Action::VolDown; a.keys[2] = Action::Mute;
    a.members = 3; a.effect = Effect::KeyCode; a.code = 0x08;

    feed(m, Action::VolUp,   State::Press, 0);
    feed(m, Action::Mute,    State::Press, 20);
    TEST_ASSERT_EQUAL(0, m.fired());
    feed(m, Action::VolDown, State::Press, 40);
    TEST_ASSERT_EQUAL(1, m.fired());
}

// ---- sequences -------------------------------------------------------------

static void armSequence(MacroEngine& m, uint8_t taps, uint16_t window, bool swallow)
{
    Macro& b = m.at(0);
    b.enabled  = true;
    b.trigger  = Trigger::Sequence;
    b.keys[0]  = Action::Mute;
    b.taps     = taps;
    b.windowMs = window;
    b.swallow  = swallow;
    b.effect   = Effect::Popup;
    strncpy(b.arg, "HI", Macro::ARG_LEN - 1);
}

static void test_three_taps_inside_the_window_fire_and_mute_nothing()
{
    MacroEngine m; reset(m); armSequence(m, 3, 500, true);

    for (uint32_t i = 0; i < 3; ++i) {
        feed(m, Action::Mute, State::Press,   i * 200);
        feed(m, Action::Mute, State::Release, i * 200 + 50);
    }
    TEST_ASSERT_EQUAL(1, m.fired());
    // THE POINT OF THE WHOLE FEATURE: "mute three times" must not mute three
    // times. Not one event reached a sink.
    TEST_ASSERT_EQUAL(0, g_outN);
}

// valid() accepts up to 10 taps, so the hold must keep 9 of them: at 12
// events a 10-tap macro overflowed on tap 7, replayed its taps in a burst and
// never fired.
static void test_the_longest_valid_sequence_fires_and_mutes_nothing()
{
    MacroEngine m; reset(m); armSequence(m, 10, 500, true);
    TEST_ASSERT_TRUE(m.at(0).valid());

    for (uint32_t i = 0; i < 10; ++i) {
        feed(m, Action::Mute, State::Press,   i * 200);
        feed(m, Action::Mute, State::Release, i * 200 + 50);
    }
    TEST_ASSERT_EQUAL(1, m.fired());
    TEST_ASSERT_EQUAL(0, m.replayed());
    TEST_ASSERT_EQUAL(0, g_outN);
}

static void test_two_taps_and_a_lapse_give_both_taps_back_in_order()
{
    MacroEngine m; reset(m); armSequence(m, 3, 500, true);

    feed(m, Action::Mute, State::Press,   0);
    feed(m, Action::Mute, State::Release, 50);
    feed(m, Action::Mute, State::Press,   200);
    feed(m, Action::Mute, State::Release, 250);
    TEST_ASSERT_EQUAL(0, g_outN);          // still holding, still might complete

    // The window lapses. Everything comes back, in the order it happened.
    g_emitN = 0;
    m.tick(900);
    for (uint8_t i = 0; i < g_emitN; ++i) g_out[g_outN++] = g_emitted[i];

    TEST_ASSERT_EQUAL(0, m.fired());
    TEST_ASSERT_EQUAL(4, g_outN);
    assertOut(0, Action::Mute, State::Press);
    assertOut(1, Action::Mute, State::Release);
    assertOut(2, Action::Mute, State::Press);
    assertOut(3, Action::Mute, State::Release);
}

static void test_a_tap_too_late_starts_a_new_sequence_rather_than_extending_one()
{
    MacroEngine m; reset(m); armSequence(m, 2, 300, true);

    feed(m, Action::Mute, State::Press,   0);
    feed(m, Action::Mute, State::Release, 40);
    m.tick(400);                                    // lapsed: the tap is given back
    feed(m, Action::Mute, State::Press,   500);
    TEST_ASSERT_EQUAL(0, m.fired());                // this is tap 1 of a new run
    feed(m, Action::Mute, State::Release, 540);
    feed(m, Action::Mute, State::Press,   600);
    TEST_ASSERT_EQUAL(1, m.fired());                // tap 2 completes it
}

static void test_the_sequence_survives_with_no_tick_at_all()
{
    // A host with no clock, or a tick that was missed: the gap check on the
    // press itself has to notice the lapse too, or a sequence spread over a
    // minute would fire.
    MacroEngine m; reset(m); armSequence(m, 2, 300, false);

    feed(m, Action::Mute, State::Press, 0);
    feed(m, Action::Mute, State::Press, 60000);
    TEST_ASSERT_EQUAL(0, m.fired());
    feed(m, Action::Mute, State::Press, 60100);
    TEST_ASSERT_EQUAL(1, m.fired());
}

static void test_a_hold_aborts_a_sequence_and_returns_the_taps()
{
    MacroEngine m; reset(m); armSequence(m, 3, 500, true);

    feed(m, Action::Mute, State::Press,   0);
    feed(m, Action::Mute, State::Release, 50);
    feed(m, Action::Mute, State::Press,   200);
    feed(m, Action::Mute, State::Long,    900);   // a finger resting, not a tap

    TEST_ASSERT_EQUAL(0, m.fired());
    // Everything held, plus the Long that broke it, in order.
    TEST_ASSERT_EQUAL(4, g_outN);
    assertOut(3, Action::Mute, State::Long);
}

static void test_a_different_key_breaks_a_sequence_and_frees_its_taps()
{
    MacroEngine m; reset(m); armSequence(m, 3, 5000, true);

    feed(m, Action::Mute,  State::Press,   0);
    feed(m, Action::Mute,  State::Release, 50);
    feed(m, Action::VolUp, State::Press,   100);

    TEST_ASSERT_EQUAL(0, m.fired());
    TEST_ASSERT_EQUAL(3, g_outN);
    assertOut(0, Action::Mute,  State::Press);
    assertOut(1, Action::Mute,  State::Release);
    assertOut(2, Action::VolUp, State::Press);
}

static void test_a_sequence_that_does_not_swallow_taps_the_key_as_well()
{
    MacroEngine m; reset(m); armSequence(m, 3, 500, false);

    for (uint32_t i = 0; i < 3; ++i) {
        feed(m, Action::Mute, State::Press,   i * 100);
        feed(m, Action::Mute, State::Release, i * 100 + 40);
    }
    TEST_ASSERT_EQUAL(1, m.fired());
    TEST_ASSERT_EQUAL(6, g_outN);      // every tap, plus the macro
}

static void test_a_completed_sequence_swallows_the_final_release_too()
{
    MacroEngine m; reset(m); armSequence(m, 2, 500, true);

    feed(m, Action::Mute, State::Press,   0);
    feed(m, Action::Mute, State::Release, 40);
    feed(m, Action::Mute, State::Press,   100);    // fires here
    feed(m, Action::Mute, State::Release, 140);    // must not escape on its own
    TEST_ASSERT_EQUAL(1, m.fired());
    TEST_ASSERT_EQUAL(0, g_outN);

    // And the key is not left muted: the next press is held as tap 1 of a new
    // run — the cost of `swallow`, paid on this key only — and comes back
    // intact when that run does not complete.
    feed(m, Action::Mute, State::Press,   2000);
    feed(m, Action::Mute, State::Release, 2040);
    TEST_ASSERT_EQUAL(0, g_outN);
    g_emitN = 0;
    m.tick(2600);
    for (uint8_t i = 0; i < g_emitN; ++i) g_out[g_outN++] = g_emitted[i];
    TEST_ASSERT_EQUAL(2, g_outN);
    assertOut(0, Action::Mute, State::Press);
    assertOut(1, Action::Mute, State::Release);
}

// A detent is a Press with no Release. Muting one would mute it for ever,
// because the release that clears a mute is never coming.
static void test_a_wheel_sequence_never_leaves_the_wheel_muted()
{
    MacroEngine m; reset(m);
    Macro& b = m.at(0);
    b.enabled = true; b.trigger = Trigger::Sequence;
    b.keys[0] = Action::EncCw; b.taps = 2; b.windowMs = 400; b.swallow = true;
    b.effect = Effect::KeyCode; b.code = 0x85;

    feed(m, Action::EncCw, State::Press, 0);
    feed(m, Action::EncCw, State::Press, 100);     // fires
    TEST_ASSERT_EQUAL(1, m.fired());

    // The very next detent must still be reachable. A muted wheel would be
    // silent for ever — the Release that clears a mute never comes for a detent
    // — so the proof is that the held tap comes back when the window lapses.
    feed(m, Action::EncCw, State::Press, 2000);
    g_emitN = 0;
    m.tick(2500);
    for (uint8_t i = 0; i < g_emitN; ++i) g_out[g_outN++] = g_emitted[i];
    TEST_ASSERT_EQUAL(1, g_outN);
    assertOut(0, Action::EncCw, State::Press);
}

// ---- hold ------------------------------------------------------------------

static void test_a_hold_macro_fires_on_long_and_releases_the_key()
{
    MacroEngine m; reset(m);
    Macro& a = m.at(0);
    a.enabled = true; a.trigger = Trigger::Hold; a.keys[0] = Action::Load;
    a.effect = Effect::Raw; a.code = 0x21; a.swallow = true;

    feed(m, Action::Load, State::Press,  0);
    feed(m, Action::Load, State::Long,   700);
    feed(m, Action::Load, State::Repeat, 850);
    feed(m, Action::Load, State::Release, 1200);

    TEST_ASSERT_EQUAL(1, m.fired());
    // Press (already gone), synthetic release. The Repeat and the real Release
    // are swallowed — the head unit was told the button came up at 700 ms.
    TEST_ASSERT_EQUAL(2, g_outN);
    assertOut(0, Action::Load, State::Press);
    assertOut(1, Action::Load, State::Release);
}

static void test_a_hold_macro_re_arms()
{
    MacroEngine m; reset(m);
    Macro& a = m.at(0);
    a.enabled = true; a.trigger = Trigger::Hold; a.keys[0] = Action::Load;
    a.effect = Effect::Raw; a.code = 0x21; a.swallow = true;

    for (uint32_t r = 0; r < 2; ++r) {
        feed(m, Action::Load, State::Press,   r * 5000);
        feed(m, Action::Load, State::Long,    r * 5000 + 700);
        feed(m, Action::Load, State::Release, r * 5000 + 1200);
    }
    TEST_ASSERT_EQUAL(2, m.fired());
}

// ---- validity, which is what the page shows next to each row ---------------

static void test_a_combo_needs_two_distinct_holdable_keys()
{
    Macro m;
    m.trigger = Trigger::Combo; m.effect = Effect::KeyCode; m.code = 1;
    m.keys[0] = Action::VolUp; m.members = 1;
    TEST_ASSERT_FALSE(m.valid());                      // one key is not a combo

    m.keys[1] = Action::VolUp; m.members = 2;
    TEST_ASSERT_FALSE(m.valid());                      // the same key twice

    m.keys[1] = Action::EncCw;
    TEST_ASSERT_FALSE(m.valid());                      // a detent is never held

    m.keys[1] = Action::VolDown;
    TEST_ASSERT_TRUE(m.valid());
}

static void test_an_effect_with_nothing_to_send_is_invalid()
{
    Macro m;
    m.trigger = Trigger::Hold; m.keys[0] = Action::Mute;

    m.effect = Effect::None;    TEST_ASSERT_FALSE(m.valid());
    m.effect = Effect::Action;  TEST_ASSERT_FALSE(m.valid());   // no action named
    m.action = Action::VolUp;   TEST_ASSERT_TRUE(m.valid());

    m.effect = Effect::Raw;     TEST_ASSERT_FALSE(m.valid());   // no command byte
    m.code = 0x21;              TEST_ASSERT_TRUE(m.valid());

    m.effect = Effect::Text;    TEST_ASSERT_FALSE(m.valid());   // no text
    strncpy(m.arg, "HELLO", Macro::ARG_LEN - 1);
    TEST_ASSERT_TRUE(m.valid());
}

static void test_a_sequence_needs_at_least_two_taps()
{
    Macro m;
    m.trigger = Trigger::Sequence; m.keys[0] = Action::Mute;
    m.effect = Effect::KeyCode; m.code = 0x07;
    m.taps = 1;   TEST_ASSERT_FALSE(m.valid());
    m.taps = 3;   TEST_ASSERT_TRUE(m.valid());
    m.taps = 11;  TEST_ASSERT_FALSE(m.valid());
}

// ---- persistence -----------------------------------------------------------

static void test_the_table_survives_a_round_trip()
{
    MacroEngine a; a.clear();
    a.at(0).enabled = true;
    a.at(0).trigger = Trigger::Combo;
    a.at(0).keys[0] = Action::VolUp; a.at(0).keys[1] = Action::VolDown;
    a.at(0).members = 2;
    a.at(0).effect = Effect::Raw; a.at(0).code = 0x21;
    a.at(0).dataLen = 3; a.at(0).data[0] = 0xAA; a.at(0).data[1] = 0xBB; a.at(0).data[2] = 0xCC;
    strncpy(a.at(0).name, "both volume", Macro::NAME_LEN - 1);
    a.at(3).enabled = true;
    a.at(3).trigger = Trigger::Sequence;
    a.at(3).keys[0] = Action::Mute; a.at(3).taps = 4; a.at(3).windowMs = 350;
    a.at(3).effect = Effect::Text;
    strncpy(a.at(3).arg, "ROW1|ROW2|ROW3", Macro::ARG_LEN - 1);

    uint8_t blob[MacroEngine::BLOB_SIZE];
    a.toBlob(blob);

    MacroEngine b; b.clear();
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));

    TEST_ASSERT_TRUE(b.at(0).enabled);
    TEST_ASSERT_EQUAL((int)Trigger::Combo, (int)b.at(0).trigger);
    TEST_ASSERT_EQUAL(0x21, b.at(0).code);
    TEST_ASSERT_EQUAL(3, b.at(0).dataLen);
    TEST_ASSERT_EQUAL(0xBB, b.at(0).data[1]);
    TEST_ASSERT_EQUAL_STRING("both volume", b.at(0).name);
    TEST_ASSERT_EQUAL(4, b.at(3).taps);
    TEST_ASSERT_EQUAL(350, b.at(3).windowMs);
    TEST_ASSERT_EQUAL_STRING("ROW1|ROW2|ROW3", b.at(3).arg);
}

static void test_a_blob_from_another_layout_is_rejected()
{
    MacroEngine a; a.loadDefaults();
    uint8_t blob[MacroEngine::BLOB_SIZE];
    a.toBlob(blob);
    blob[0] = 0x7F;                                   // a version nothing wrote

    MacroEngine b; b.clear();
    TEST_ASSERT_FALSE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_FALSE(b.fromBlob(blob, sizeof(blob) - 1));
}

// A blob is not a trusted document: it can come from an older layout, a partial
// write, or a namespace another sketch used. Nothing out of it may become an
// out-of-range enum that later indexes a name table.
static void test_a_corrupt_blob_cannot_produce_an_out_of_range_enum()
{
    uint8_t blob[MacroEngine::BLOB_SIZE];
    MacroEngine a; a.clear();
    a.toBlob(blob);
    blob[2]  = 200;                                   // trigger
    blob[10] = 200;                                   // effect (offset within record 0)
    for (uint16_t i = 3; i < 6; ++i) blob[i] = 200;   // member actions

    MacroEngine b;
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_EQUAL((int)Trigger::Off, (int)b.at(0).trigger);
    TEST_ASSERT_EQUAL((int)Action::None, (int)b.at(0).keys[0]);
    TEST_ASSERT_FALSE(b.at(0).valid());
}

static void test_the_shipped_defaults_are_switched_off()
{
    MacroEngine m; m.loadDefaults();
    for (uint8_t i = 0; i < MacroEngine::count(); ++i)
        TEST_ASSERT_FALSE_MESSAGE(m.at(i).enabled,
                                  "a macro that arrives enabled is a button nobody asked for");
    // ...and the two examples are nonetheless complete enough to switch on.
    TEST_ASSERT_TRUE(m.at(0).valid());
    TEST_ASSERT_TRUE(m.at(1).valid());
}

static void test_the_names_round_trip()
{
    TEST_ASSERT_EQUAL((int)Trigger::Sequence, (int)triggerFromName("sequence"));
    TEST_ASSERT_EQUAL((int)Trigger::Off,      (int)triggerFromName("nonsense"));
    TEST_ASSERT_EQUAL((int)Trigger::Off,      (int)triggerFromName(nullptr));
    TEST_ASSERT_EQUAL_STRING("combo", triggerName(Trigger::Combo));

    TEST_ASSERT_EQUAL((int)Effect::Message, (int)effectFromName("message"));
    TEST_ASSERT_EQUAL((int)Effect::None,    (int)effectFromName("nonsense"));
    TEST_ASSERT_EQUAL_STRING("key", effectName(Effect::KeyCode));
}

// ---- one task, a staged table (Step 2) ---------------------------------------
// The engine has no lock any more: the keys task is its only caller, and the
// web edits a MacroTable that the keys task swaps in with setTable().

static void test_a_new_table_keeps_each_rows_fire_count()
{
    MacroEngine m; reset(m); armCombo(m, true);
    feed(m, Action::VolUp,   State::Press, 0);
    feed(m, Action::VolDown, State::Press, 30);
    TEST_ASSERT_EQUAL(1, m.at(0).fires);

    MacroTable t;
    m.table(t);
    t.m[0].code = 0x08;                     // an edit from the page
    t.m[0].fires = 99;                      // whatever the page had: not configuration
    m.setTable(t);
    TEST_ASSERT_EQUAL(0x08, m.at(0).code);
    TEST_ASSERT_EQUAL(1, m.at(0).fires);
}

static void test_a_new_table_drops_mutes_from_the_old_one()
{
    MacroEngine m; reset(m); armCombo(m, true);
    feed(m, Action::VolUp,   State::Press, 0);
    feed(m, Action::VolDown, State::Press, 30);          // VOL- muted until released
    MacroTable t;                                         // every row gone
    m.setTable(t);
    g_outN = 0;
    feed(m, Action::VolDown, State::Release, 80);
    feed(m, Action::VolDown, State::Press, 120);
    // With the macro deleted, the key is a key again.
    TEST_ASSERT_EQUAL(2, g_outN);
    assertOut(1, Action::VolDown, State::Press);
}

static void test_a_table_round_trips_through_the_blob_on_its_own()
{
    MacroTable a; a.loadDefaults();
    a.m[3].enabled = true; a.m[3].trigger = Trigger::Hold;
    a.m[3].keys[0] = Action::Phone; a.m[3].effect = Effect::Message; a.m[3].code = 4;
    strncpy(a.m[3].name, "hold phone", Macro::NAME_LEN - 1);
    uint8_t blob[MacroEngine::BLOB_SIZE];
    MacroEngine::toBlob(a, blob);
    MacroTable b;
    TEST_ASSERT_TRUE(MacroEngine::fromBlob(b, blob, sizeof(blob)));
    TEST_ASSERT_TRUE(b.m[3].enabled);
    TEST_ASSERT_EQUAL((int)Trigger::Hold, (int)b.m[3].trigger);
    TEST_ASSERT_EQUAL_STRING("hold phone", b.m[3].name);
    // And the engine reads the same bytes the same way.
    MacroEngine e;
    TEST_ASSERT_TRUE(e.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_EQUAL(4, e.at(3).code);
}

static void test_run_performs_the_effect_without_touching_the_counters()
{
    MacroEngine m; reset(m); armCombo(m, true);
    m.run(0);
    TEST_ASSERT_EQUAL(1, g_fireN);
    TEST_ASSERT_EQUAL(0x07, g_fired[0].code);
    TEST_ASSERT_EQUAL(0, m.fired());
    TEST_ASSERT_EQUAL(0, m.at(0).fires);
}

static void test_held_taps_tell_the_keys_task_when_to_wake()
{
    MacroEngine m; reset(m);
    Macro& s = m.at(0);
    s.enabled = true; s.trigger = Trigger::Sequence; s.keys[0] = Action::Mute;
    s.taps = 3; s.windowMs = 400; s.swallow = true; s.effect = Effect::Popup;
    strncpy(s.arg, "X", Macro::ARG_LEN - 1);
    uint32_t due = 0;
    TEST_ASSERT_FALSE(m.nextDue(due));
    feed(m, Action::Mute, State::Press, 1000);
    TEST_ASSERT_TRUE(m.nextDue(due));
    TEST_ASSERT_EQUAL(1400u, due);
    m.tick(1400);                                         // window lapsed: replayed
    TEST_ASSERT_FALSE(m.nextDue(due));
}

// The executor posts rather than acts for everything the keys task does not
// own. The route is decided here, in portable code, so it is tested.
static void test_each_effect_names_the_task_that_performs_it()
{
    TEST_ASSERT_EQUAL((int)EffectRoute::Keys,    (int)effectRoute(Effect::Action));
    TEST_ASSERT_EQUAL((int)EffectRoute::Keys,    (int)effectRoute(Effect::KeyCode));
    TEST_ASSERT_EQUAL((int)EffectRoute::UartOut, (int)effectRoute(Effect::Raw));
    TEST_ASSERT_EQUAL((int)EffectRoute::Hu,      (int)effectRoute(Effect::Message));
    TEST_ASSERT_EQUAL((int)EffectRoute::Panel,   (int)effectRoute(Effect::Text));
    TEST_ASSERT_EQUAL((int)EffectRoute::Panel,   (int)effectRoute(Effect::Popup));
    TEST_ASSERT_EQUAL((int)EffectRoute::Pane,    (int)effectRoute(Effect::Pane));
    TEST_ASSERT_EQUAL((int)EffectRoute::None,    (int)effectRoute(Effect::None));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_empty_table_passes_everything_through);
    RUN_TEST(test_a_disabled_macro_is_not_a_macro);

    RUN_TEST(test_a_combo_fires_when_the_second_member_lands);
    RUN_TEST(test_a_swallowed_combo_releases_the_member_it_already_sent);
    RUN_TEST(test_the_real_releases_of_a_swallowed_combo_never_reach_a_sink);
    RUN_TEST(test_a_combo_re_arms_after_the_fingers_come_off);
    RUN_TEST(test_a_combo_does_not_re_fire_while_it_is_still_held);
    RUN_TEST(test_a_combo_that_does_not_swallow_lets_both_keys_through);
    RUN_TEST(test_a_partial_combo_is_just_two_ordinary_keys);
    RUN_TEST(test_a_three_key_combo_needs_all_three);

    RUN_TEST(test_three_taps_inside_the_window_fire_and_mute_nothing);
    RUN_TEST(test_the_longest_valid_sequence_fires_and_mutes_nothing);
    RUN_TEST(test_two_taps_and_a_lapse_give_both_taps_back_in_order);
    RUN_TEST(test_a_tap_too_late_starts_a_new_sequence_rather_than_extending_one);
    RUN_TEST(test_the_sequence_survives_with_no_tick_at_all);
    RUN_TEST(test_a_hold_aborts_a_sequence_and_returns_the_taps);
    RUN_TEST(test_a_different_key_breaks_a_sequence_and_frees_its_taps);
    RUN_TEST(test_a_sequence_that_does_not_swallow_taps_the_key_as_well);
    RUN_TEST(test_a_completed_sequence_swallows_the_final_release_too);
    RUN_TEST(test_a_wheel_sequence_never_leaves_the_wheel_muted);

    RUN_TEST(test_a_hold_macro_fires_on_long_and_releases_the_key);
    RUN_TEST(test_a_hold_macro_re_arms);

    RUN_TEST(test_a_combo_needs_two_distinct_holdable_keys);
    RUN_TEST(test_an_effect_with_nothing_to_send_is_invalid);
    RUN_TEST(test_a_sequence_needs_at_least_two_taps);

    RUN_TEST(test_the_table_survives_a_round_trip);
    RUN_TEST(test_a_blob_from_another_layout_is_rejected);
    RUN_TEST(test_a_corrupt_blob_cannot_produce_an_out_of_range_enum);
    RUN_TEST(test_the_shipped_defaults_are_switched_off);
    RUN_TEST(test_the_names_round_trip);
    RUN_TEST(test_a_new_table_keeps_each_rows_fire_count);
    RUN_TEST(test_a_new_table_drops_mutes_from_the_old_one);
    RUN_TEST(test_a_table_round_trips_through_the_blob_on_its_own);
    RUN_TEST(test_run_performs_the_effect_without_touching_the_counters);
    RUN_TEST(test_held_taps_tell_the_keys_task_when_to_wake);
    RUN_TEST(test_each_effect_names_the_task_that_performs_it);
    return UNITY_END();
}
