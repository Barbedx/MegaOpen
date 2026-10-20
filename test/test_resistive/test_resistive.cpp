#include <unity.h>
#include "sinks/ResistiveLadder.h"
#include "sinks/ResistiveMap.h"
#include "sinks/ResistiveKeySink.h"

using namespace Sinks;
using namespace Keys;

// ---------------------------------------------------------------------------
// A recording port. Everything this sink does leaves through one callback, so
// the whole press choreography is checkable without a head unit, a multimeter
// or a car — which is the point of keeping it portable.
// ---------------------------------------------------------------------------
struct Applied { uint8_t ch; uint8_t mask; };
static Applied g_log[64];
static uint8_t g_n = 0;

static void recordApply(uint8_t ch, uint8_t mask, void*)
{
    if (g_n < 64) g_log[g_n++] = Applied{ch, mask};
}
static void resetLog() { g_n = 0; }

static ResistiveMap     g_map;
static ResistiveKeySink g_sink;

void setUp(void) {}
void tearDown(void) {}

static void freshSink(uint8_t classes = Keys::CLASS_ALL)
{
    g_map.loadDefaults();
    g_sink.begin(recordApply, nullptr, &g_map);
    g_sink.setEnabled(true);
    g_sink.setClasses(classes);
    resetLog();
}

static Event ev(Action a, State s, uint32_t t)
{
    Event e; e.action = a; e.state = s; e.tMs = t; return e;
}

// ---------------------------------------------------------------------------
// The ladder: arithmetic
// ---------------------------------------------------------------------------
static void test_a_single_leg_is_its_own_resistor_plus_the_series_term(void)
{
    ResistiveLadder l;
    l.setLegCount(4);
    l.setLeg(0, 560); l.setLeg(1, 1500); l.setLeg(2, 3900); l.setLeg(3, 10000);
    l.setSeriesOhms(30);

    TEST_ASSERT_EQUAL_UINT16(590,   l.ohmsFor(0x01));
    TEST_ASSERT_EQUAL_UINT16(1530,  l.ohmsFor(0x02));
    TEST_ASSERT_EQUAL_UINT16(3930,  l.ohmsFor(0x04));
    TEST_ASSERT_EQUAL_UINT16(10030, l.ohmsFor(0x08));
}

static void test_two_legs_are_in_parallel(void)
{
    ResistiveLadder l;
    l.setLegCount(2);
    l.setLeg(0, 1000); l.setLeg(1, 1000);
    l.setSeriesOhms(0);
    // 1k || 1k = 500. If this ever reads 2000 somebody has written series.
    TEST_ASSERT_EQUAL_UINT16(500, l.ohmsFor(0x03));
}

static void test_no_leg_engaged_is_open_and_not_zero_ohms(void)
{
    ResistiveLadder l;
    l.setLegCount(4);
    l.setLeg(0, 560);
    // THE DISTINCTION THIS WHOLE SENTINEL EXISTS FOR: an open line is "no key",
    // and zero ohms is "every button at once".
    TEST_ASSERT_EQUAL_UINT16(ResistiveLadder::OPEN, l.ohmsFor(0x00));
}

static void test_a_mask_pointing_at_unfitted_legs_reads_open(void)
{
    ResistiveLadder l;
    l.setLegCount(4);
    l.setLeg(0, 560);
    // Legs 1..3 never populated. A naive sum would divide by zero; a naive
    // clamp would report 0 ohm, i.e. a short.
    TEST_ASSERT_EQUAL_UINT16(ResistiveLadder::OPEN, l.ohmsFor(0x0E));
}

static void test_a_leg_below_the_floor_is_clamped_not_ignored(void)
{
    ResistiveLadder l;
    l.setLegCount(1);
    l.setLeg(0, 5);                       // somebody typing in the web UI
    TEST_ASSERT_EQUAL_UINT16(ResistiveLadder::MIN_OHMS, l.leg(0));
}

// ---------------------------------------------------------------------------
// The ladder: choosing rungs
// ---------------------------------------------------------------------------
static void test_four_buttons_on_four_legs_get_one_leg_each(void)
{
    ResistiveLadder l;
    l.setLegCount(4);
    // All four INSIDE the window. An earlier version of this test used 10k for
    // the top leg and passed, which is precisely the mistake the window check
    // now catches: it is above the ~5k ceiling, so it is not a rung at all.
    l.setLeg(0, 220); l.setLeg(1, 560); l.setLeg(2, 1500); l.setLeg(3, 3900);

    uint8_t m[8];
    TEST_ASSERT_EQUAL_UINT8(4, l.chooseMasks(4, m));
    // Single bits, ascending in resistance — NOT parallel combinations, which
    // is the whole point: the combinations bunch and these four do not.
    TEST_ASSERT_EQUAL_UINT8(0x01, m[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, m[1]);
    TEST_ASSERT_EQUAL_UINT8(0x04, m[2]);
    TEST_ASSERT_EQUAL_UINT8(0x08, m[3]);
}

static void test_one_leg_per_button_is_far_better_separated_than_combinations(void)
{
    ResistiveLadder l;
    l.setLegCount(4);
    l.setLeg(0, 220); l.setLeg(1, 560); l.setLeg(2, 1500); l.setLeg(3, 3900);

    uint8_t single[4];
    l.chooseMasks(4, single);
    // The four resistors themselves are >150% apart.
    TEST_ASSERT_GREATER_THAN_UINT8(100, l.worstSeparationPct(single, 4));

    // ...whereas the four TIGHTEST combinations are within a few percent, which
    // is the head unit learning two buttons as one. This is the failure the
    // single-leg preference exists to avoid, asserted rather than asserted-about.
    const uint8_t bunched[4] = {0x0F, 0x07, 0x0B, 0x0D};
    TEST_ASSERT_LESS_THAN_UINT8(30, l.worstSeparationPct(bunched, 4));
}

static void test_more_buttons_than_legs_falls_back_to_combinations(void)
{
    ResistiveLadder l;
    l.setLegCount(4);
    l.setLeg(0, 560); l.setLeg(1, 1500); l.setLeg(2, 3900); l.setLeg(3, 10000);

    uint8_t m[16];
    const uint8_t n = l.chooseMasks(6, m);
    TEST_ASSERT_EQUAL_UINT8(6, n);
    // Every one distinct — a repeat here would be two buttons on one rung.
    for (uint8_t i = 0; i < n; ++i)
        for (uint8_t j = (uint8_t)(i + 1); j < n; ++j)
            TEST_ASSERT_NOT_EQUAL(m[i], m[j]);
}

static void test_a_rung_outside_the_window_is_never_handed_out(void)
{
    ResistiveLadder l;
    l.setLegCount(5);
    l.setLeg(0, 150); l.setLeg(1, 390); l.setLeg(2, 820);
    l.setLeg(3, 1800); l.setLeg(4, 3300);
    l.setSeriesOhms(2);

    uint8_t m[32];
    const uint8_t n = l.chooseMasks(8, m);
    TEST_ASSERT_EQUAL_UINT8(8, n);
    for (uint8_t i = 0; i < n; ++i) {
        const uint16_t r = l.ohmsFor(m[i]);
        // All five legs in parallel is 90 ohm on this ladder — below the floor
        // where these inputs separate cleanly. The old spread handed it out
        // because it was the widest thing available.
        TEST_ASSERT_GREATER_OR_EQUAL_UINT16(ResistiveLadder::USABLE_MIN_OHMS, r);
        TEST_ASSERT_LESS_OR_EQUAL_UINT16(ResistiveLadder::USABLE_MAX_OHMS, r);
    }
}

static void test_the_spread_maximises_the_worst_ratio_not_the_linear_gap(void)
{
    ResistiveLadder l;
    l.setLegCount(5);
    l.setLeg(0, 150); l.setLeg(1, 390); l.setLeg(2, 820);
    l.setLeg(3, 1800); l.setLeg(4, 3300);
    l.setSeriesOhms(2);

    // THE REGRESSION THIS GUARDS. Walking LINEAR targets between the lowest and
    // highest rung looks reasonable and is wrong, because resistance is a ratio
    // scale: on this ladder it put five of six targets above 700 ohm, skipped
    // the 152 and 392 ohm single-leg rungs entirely, and returned a set whose
    // tightest pair was 24% apart. Maximising the ratio gives 80%.
    uint8_t m6[8];
    TEST_ASSERT_EQUAL_UINT8(6, l.chooseMasks(6, m6));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT8(70, l.worstSeparationPct(m6, 6));

    // Eight buttons — the real Mégane count — must still clear the margin where
    // thermal drift starts swapping neighbours.
    uint8_t m8[8];
    TEST_ASSERT_EQUAL_UINT8(8, l.chooseMasks(8, m8));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT8(40, l.worstSeparationPct(m8, 8));
}

static void test_two_actions_on_one_rung_report_zero_separation(void)
{
    ResistiveLadder l;
    l.setLegCount(4);
    l.setLeg(0, 560); l.setLeg(1, 1500);
    const uint8_t same[2] = {0x01, 0x01};
    // Not "a very small percentage" — a collision, which has to be visibly
    // different from a tight-but-workable gap on the settings page.
    TEST_ASSERT_EQUAL_UINT8(0, l.worstSeparationPct(same, 2));
}

// ---------------------------------------------------------------------------
// The map
// ---------------------------------------------------------------------------
static void test_nothing_is_assigned_by_default(void)
{
    ResistiveMap m;
    m.loadDefaults();
    for (uint8_t i = 1; i < ResistiveMap::ACTIONS; ++i)
        TEST_ASSERT_EQUAL_UINT8(0, m.slot((Action)i).mask);
}

static void test_an_encoder_phase_can_never_be_given_a_rung(void)
{
    ResistiveMap m;
    m.loadDefaults();
    m.setSlot(Action::EncPhaseA, ResistiveMap::Slot{0, 0x01});
    // A phase emits no Release, so a rung on one would be a resistance nothing
    // could ever lift.
    TEST_ASSERT_EQUAL_UINT8(0, m.slot(Action::EncPhaseA).mask);
}

static void test_the_map_survives_a_blob_round_trip(void)
{
    ResistiveMap a;
    a.loadDefaults();
    a.setSlot(Action::VolUp,   ResistiveMap::Slot{0, 0x01});
    a.setSlot(Action::VolDown, ResistiveMap::Slot{0, 0x02});
    a.setSlot(Action::Mode,    ResistiveMap::Slot{1, 0x04});
    a.ladder(1).setLeg(2, 2200);
    a.timing().pulseMs = 77;
    a.timing().repeatStartMs = 480;
    a.setSpin(ResistiveMap::Spin::Pulse);
    a.setActiveHigh(true);

    uint8_t blob[ResistiveMap::BLOB_SIZE];
    a.toBlob(blob);

    ResistiveMap b;
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT8(0x01, b.slot(Action::VolUp).mask);
    TEST_ASSERT_EQUAL_UINT8(1,    b.slot(Action::Mode).ch);
    TEST_ASSERT_EQUAL_UINT8(0x04, b.slot(Action::Mode).mask);
    TEST_ASSERT_EQUAL_UINT16(2200, b.ladder(1).leg(2));
    TEST_ASSERT_EQUAL_UINT16(77,  b.timing().pulseMs);
    TEST_ASSERT_EQUAL_UINT16(480, b.timing().repeatStartMs);
    TEST_ASSERT_EQUAL_INT((int)ResistiveMap::Spin::Pulse, (int)b.spin());
    TEST_ASSERT_TRUE(b.activeHigh());
}

static void test_a_blob_of_the_wrong_length_is_rejected_whole(void)
{
    uint8_t blob[ResistiveMap::BLOB_SIZE] = {};
    blob[0] = 1;
    ResistiveMap b;
    TEST_ASSERT_FALSE(b.fromBlob(blob, ResistiveMap::BLOB_SIZE - 1));
}

static void test_a_stored_zero_hold_guard_is_repaired_on_load(void)
{
    ResistiveMap a;
    a.loadDefaults();
    a.timing().maxHoldMs = 0;             // the one invalid value that is unsafe
    uint8_t blob[ResistiveMap::BLOB_SIZE];
    a.toBlob(blob);

    ResistiveMap b;
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_NOT_EQUAL(0, b.timing().maxHoldMs);
}

static void test_auto_assign_is_idempotent(void)
{
    ResistiveMap m;
    m.loadDefaults();
    const Action want[] = {Action::VolUp, Action::VolDown, Action::SrcNext, Action::Mode};

    m.autoAssign(0, want, 4);
    const uint8_t first = m.slot(Action::Mode).mask;
    m.autoAssign(0, want, 4);
    // Re-running must not repack around what the previous run left behind —
    // otherwise the spread degrades every time somebody presses the button.
    TEST_ASSERT_EQUAL_UINT8(first, m.slot(Action::Mode).mask);
}

// ---------------------------------------------------------------------------
// The sink: levels, not events
// ---------------------------------------------------------------------------
static void test_a_press_engages_and_a_release_opens(void)
{
    freshSink();
    g_map.setSlot(Action::Mode, ResistiveMap::Slot{0, 0x02});

    g_sink.onKey(ev(Action::Mode, State::Press, 1000));
    TEST_ASSERT_EQUAL_UINT8(1, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x02, g_log[0].mask);

    g_sink.onKey(ev(Action::Mode, State::Release, 1200));
    TEST_ASSERT_EQUAL_UINT8(2, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_log[1].mask);
}

static void test_long_and_repeat_do_not_touch_the_line(void)
{
    freshSink();
    g_map.setSlot(Action::VolUp, ResistiveMap::Slot{0, 0x01});

    g_sink.onKey(ev(Action::VolUp, State::Press,  1000));
    resetLog();
    g_sink.onKey(ev(Action::VolUp, State::Long,   1600));
    g_sink.onKey(ev(Action::VolUp, State::Repeat, 1750));
    g_sink.onKey(ev(Action::VolUp, State::Repeat, 1900));
    // The rung is ALREADY held. The head unit is doing its own long-press and
    // its own repeat on top of it; re-applying would be pressing a button that
    // was never released.
    TEST_ASSERT_EQUAL_UINT8(0, g_n);
}

static void test_an_unassigned_action_is_silent(void)
{
    freshSink();
    g_sink.onKey(ev(Action::Voice, State::Press, 1000));
    TEST_ASSERT_EQUAL_UINT8(0, g_n);
    TEST_ASSERT_EQUAL_STRING("no rung", g_sink.explain(ev(Action::Voice, State::Press, 0)).why);
}

static void test_a_rung_on_unfitted_legs_says_so_rather_than_acting(void)
{
    freshSink();
    g_map.ladder(0).setLeg(1, 0);                     // leg not populated
    g_map.setSlot(Action::Mode, ResistiveMap::Slot{0, 0x02});
    g_sink.onKey(ev(Action::Mode, State::Press, 1000));
    TEST_ASSERT_EQUAL_UINT8(0, g_n);
    TEST_ASSERT_EQUAL_STRING("legs unfitted",
                             g_sink.explain(ev(Action::Mode, State::Press, 0)).why);
}

static void test_routing_off_means_silent(void)
{
    freshSink(Keys::CLASS_VOLUME);
    g_map.setSlot(Action::Mode, ResistiveMap::Slot{0, 0x02});   // Mode is CLASS_UI
    g_sink.onKey(ev(Action::Mode, State::Press, 1000));
    TEST_ASSERT_EQUAL_UINT8(0, g_n);
    TEST_ASSERT_EQUAL_STRING("not routed",
                             g_sink.explain(ev(Action::Mode, State::Press, 0)).why);
}

static void test_disabling_the_sink_releases_a_held_key(void)
{
    freshSink();
    g_map.setSlot(Action::VolUp, ResistiveMap::Slot{0, 0x01});
    g_sink.onKey(ev(Action::VolUp, State::Press, 1000));
    resetLog();

    g_sink.setEnabled(false);
    // Switching off must RELEASE, not merely stop sending: the difference is a
    // resistance that stays on the wire until the next reboot.
    TEST_ASSERT_EQUAL_UINT8(1, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_log[0].mask);
}

static void test_a_lost_release_is_broken_by_the_stuck_guard(void)
{
    freshSink();
    g_map.setSlot(Action::VolUp, ResistiveMap::Slot{0, 0x01});
    g_sink.onKey(ev(Action::VolUp, State::Press, 1000));
    resetLog();

    g_sink.tick(1000 + g_map.timing().maxHoldMs - 1);
    TEST_ASSERT_EQUAL_UINT8(0, g_n);                  // still legitimately held

    g_sink.tick(1000 + g_map.timing().maxHoldMs + 1);
    TEST_ASSERT_EQUAL_UINT8(1, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_log[0].mask);
    // Counted, because a non-zero value here is a fault UPSTREAM of this class
    // and is invisible everywhere else.
    TEST_ASSERT_EQUAL_UINT32(1, g_sink.stuck());
}

// ---------------------------------------------------------------------------
// The sink: one channel, one voice
// ---------------------------------------------------------------------------
static void test_two_rungs_are_never_engaged_at_once(void)
{
    freshSink();
    g_map.setSlot(Action::Mode,  ResistiveMap::Slot{0, 0x02});
    g_map.setSlot(Action::Radio, ResistiveMap::Slot{0, 0x04});

    g_sink.onKey(ev(Action::Mode,  State::Press, 1000));
    g_sink.onKey(ev(Action::Radio, State::Press, 1050));
    // Mode must be lifted before Radio lands — two rungs in parallel is a THIRD
    // resistance, i.e. a button nobody pressed.
    TEST_ASSERT_EQUAL_UINT8(3, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x02, g_log[0].mask);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_log[1].mask);
    TEST_ASSERT_EQUAL_UINT8(0x04, g_log[2].mask);
}

static void test_a_release_from_a_preempted_key_does_not_lift_the_new_one(void)
{
    freshSink();
    g_map.setSlot(Action::Mode,  ResistiveMap::Slot{0, 0x02});
    g_map.setSlot(Action::Radio, ResistiveMap::Slot{0, 0x04});

    g_sink.onKey(ev(Action::Mode,  State::Press, 1000));
    g_sink.onKey(ev(Action::Radio, State::Press, 1050));
    resetLog();
    g_sink.onKey(ev(Action::Mode,  State::Release, 1100));
    // Mode lost the channel. Its late Release must not open Radio's rung.
    TEST_ASSERT_EQUAL_UINT8(0, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x04, g_sink.liveMask(0));
}

static void test_the_two_channels_are_independent(void)
{
    freshSink();
    g_map.setSlot(Action::Mode,  ResistiveMap::Slot{0, 0x02});
    g_map.setSlot(Action::Radio, ResistiveMap::Slot{1, 0x02});

    g_sink.onKey(ev(Action::Mode,  State::Press, 1000));
    g_sink.onKey(ev(Action::Radio, State::Press, 1050));
    // Different inputs on the head unit (G7 and G6): no preemption, both held.
    TEST_ASSERT_EQUAL_UINT8(0x02, g_sink.liveMask(0));
    TEST_ASSERT_EQUAL_UINT8(0x02, g_sink.liveMask(1));
    TEST_ASSERT_EQUAL_UINT32(0, g_sink.preempts());
}

// ---------------------------------------------------------------------------
// The sink: the wheel
// ---------------------------------------------------------------------------
static void test_a_detent_pulses_and_releases_itself(void)
{
    freshSink();
    g_map.setSpin(ResistiveMap::Spin::Pulse);
    g_map.setSlot(Action::EncCw, ResistiveMap::Slot{0, 0x01});

    g_sink.onKey(ev(Action::EncCw, State::Press, 1000));
    // Queued, not applied — the queue is drained on tick, which is what keeps a
    // fast spin from turning into a blocking delay in the key path.
    TEST_ASSERT_EQUAL_UINT8(0, g_n);

    g_sink.tick(1000);
    TEST_ASSERT_EQUAL_UINT8(1, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x01, g_log[0].mask);

    g_sink.tick(1000 + g_map.timing().pulseMs + 1);
    TEST_ASSERT_EQUAL_UINT8(2, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_log[1].mask);     // released by the clock
}

static void test_the_gap_is_enforced_between_two_detents(void)
{
    freshSink();
    g_map.setSpin(ResistiveMap::Spin::Pulse);
    g_map.setSlot(Action::EncCw, ResistiveMap::Slot{0, 0x01});

    g_sink.onKey(ev(Action::EncCw, State::Press, 1000));
    g_sink.onKey(ev(Action::EncCw, State::Press, 1005));
    g_sink.tick(1000);                                 // first press
    g_sink.tick(1000 + g_map.timing().pulseMs + 1);    // first release
    resetLog();

    g_sink.tick(1000 + g_map.timing().pulseMs + 2);
    // Too soon. The head unit has to SEE the idle level between two presses or
    // it reads one long one.
    TEST_ASSERT_EQUAL_UINT8(0, g_n);

    g_sink.tick(1000 + g_map.timing().pulseMs + g_map.timing().gapMs + 4);
    TEST_ASSERT_EQUAL_UINT8(1, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x01, g_log[0].mask);
}

static void test_reversing_the_wheel_drops_the_pending_steps(void)
{
    freshSink();
    g_map.setSpin(ResistiveMap::Spin::Pulse);
    g_map.setSlot(Action::EncCw,  ResistiveMap::Slot{0, 0x01});
    g_map.setSlot(Action::EncCcw, ResistiveMap::Slot{0, 0x02});

    for (uint8_t i = 0; i < 5; ++i)
        g_sink.onKey(ev(Action::EncCw, State::Press, 1000 + i));
    TEST_ASSERT_EQUAL_UINT8(5, g_sink.queued(0));

    g_sink.onKey(ev(Action::EncCcw, State::Press, 1010));
    // Turning back means the driver has already passed what they wanted.
    // Delivering the backlog first would walk further away from it.
    TEST_ASSERT_EQUAL_UINT8(1, g_sink.queued(0));
}

static void test_the_queue_has_a_ceiling_and_counts_what_it_drops(void)
{
    freshSink();
    g_map.setSpin(ResistiveMap::Spin::Pulse);
    g_map.setSlot(Action::EncCw, ResistiveMap::Slot{0, 0x01});

    for (uint8_t i = 0; i < ResistiveKeySink::QUEUE_MAX + 5; ++i)
        g_sink.onKey(ev(Action::EncCw, State::Press, 1000 + i));

    TEST_ASSERT_EQUAL_UINT8(ResistiveKeySink::QUEUE_MAX, g_sink.queued(0));
    // Silent truncation would read as "the wheel is fine" while steps vanish.
    TEST_ASSERT_EQUAL_UINT32(5, g_sink.dropped());
}

static void test_a_button_press_preempts_a_spinning_wheel(void)
{
    freshSink();
    g_map.setSpin(ResistiveMap::Spin::Pulse);
    g_map.setSlot(Action::EncCw, ResistiveMap::Slot{0, 0x01});
    g_map.setSlot(Action::Mode,  ResistiveMap::Slot{0, 0x04});

    for (uint8_t i = 0; i < 6; ++i)
        g_sink.onKey(ev(Action::EncCw, State::Press, 1000 + i));
    g_sink.tick(1000);
    resetLog();

    g_sink.onKey(ev(Action::Mode, State::Press, 1010));
    // A volume trim can wait; a SOURCE press that waits behind it is a button
    // the driver reports as "not working".
    TEST_ASSERT_EQUAL_UINT8(0, g_sink.queued(0));
    TEST_ASSERT_EQUAL_UINT8(0x04, g_sink.liveMask(0));
    TEST_ASSERT_EQUAL_UINT32(1, g_sink.preempts());
}

static void test_hybrid_holds_only_once_the_queue_saturates(void)
{
    freshSink();
    g_map.setSpin(ResistiveMap::Spin::Hybrid);
    g_map.setSlot(Action::EncCw, ResistiveMap::Slot{0, 0x01});

    // Below the threshold it is exactly Pulse — which is the point: half a
    // second of dead time before the head unit repeats anything would make a
    // one-click nudge feel broken.
    for (uint8_t i = 0; i < 3; ++i)
        g_sink.onKey(ev(Action::EncCw, State::Press, 1000 + i));
    g_sink.tick(1000);
    TEST_ASSERT_EQUAL_UINT8(2, g_sink.queued(0));      // one taken, two left
}

static void test_a_hybrid_hold_lasts_the_dead_time_plus_one_period_per_detent(void)
{
    freshSink();
    g_map.setSpin(ResistiveMap::Spin::Hybrid);
    g_map.setSlot(Action::EncCw, ResistiveMap::Slot{0, 0x01});

    const uint8_t n = g_map.timing().holdAfter;
    for (uint8_t i = 0; i < n; ++i)
        g_sink.onKey(ev(Action::EncCw, State::Press, 1000 + i));

    g_sink.tick(1000);
    TEST_ASSERT_EQUAL_UINT8(0x01, g_sink.liveMask(0));
    TEST_ASSERT_EQUAL_UINT8(0, g_sink.queued(0));       // absorbed into the hold

    // A FIXED hold would deliver the head unit's repeat rate for its whole
    // length — 20 volume steps for the 8 the driver asked for. The length is
    // computed instead: 500 ms of dead time plus 50 ms per detent owed.
    const uint32_t want = (uint32_t)g_map.timing().repeatStartMs
                        + (uint32_t)n * g_map.timing().repeatMs;
    g_sink.tick(1000 + want - 2);
    TEST_ASSERT_EQUAL_UINT8(0x01, g_sink.liveMask(0));  // still held
    g_sink.tick(1000 + want + 2);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_sink.liveMask(0));  // released on time
}

// ---------------------------------------------------------------------------
// Learn assist — the >3 s steady hold the head unit's study page requires
// ---------------------------------------------------------------------------
static void test_the_default_ladder_is_inside_the_units_usable_window(void)
{
    ResistiveMap m;
    m.loadDefaults();
    // The shipped values must be teachable AND still work afterwards. A rung
    // above ~5k learns perfectly and then reads as an open line for ever.
    TEST_ASSERT_TRUE(m.ladder(0).inUsableWindow());
}

static void test_a_leg_above_the_window_is_flagged_not_silently_accepted(void)
{
    ResistiveLadder l;
    l.setLegCount(2);
    l.setLeg(0, 560);
    l.setLeg(1, 10000);                  // the value an earlier draft shipped
    TEST_ASSERT_FALSE(l.inUsableWindow());
}

static void test_an_encoder_direction_can_be_taught_which_a_detent_never_could(void)
{
    freshSink();
    g_map.setSlot(Action::EncCw, ResistiveMap::Slot{0, 0x04});

    // THE CASE THIS FEATURE EXISTS FOR. A detent is a 60 ms pulse; the study
    // page wants more than three seconds of steady resistance. Without an
    // explicit assert the wheel is not hard to teach, it is unteachable.
    TEST_ASSERT_TRUE(g_sink.learnAction(Action::EncCw, ResistiveKeySink::LEARN_MS, 5000));
    TEST_ASSERT_TRUE(g_sink.learning(0));
    TEST_ASSERT_EQUAL_UINT8(0x04, g_sink.liveMask(0));

    g_sink.tick(5000 + 3000);            // three seconds in, still flat
    TEST_ASSERT_EQUAL_UINT8(0x04, g_sink.liveMask(0));

    g_sink.tick(5000 + ResistiveKeySink::LEARN_MS + 1);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_sink.liveMask(0));   // clean break at the end
    TEST_ASSERT_FALSE(g_sink.learning(0));
}

static void test_a_learn_hold_ignores_routing_so_setup_is_not_circular(void)
{
    freshSink(Keys::CLASS_NONE);         // nothing routed here at all
    g_map.setSlot(Action::Mode, ResistiveMap::Slot{0, 0x02});
    // You have to be able to teach a button BEFORE switching its class on.
    TEST_ASSERT_TRUE(g_sink.learnAction(Action::Mode, 4000, 1000));
    TEST_ASSERT_EQUAL_UINT8(0x02, g_sink.liveMask(0));
}

static void test_nothing_disturbs_a_learn_hold(void)
{
    freshSink();
    g_map.setSlot(Action::EncCw, ResistiveMap::Slot{0, 0x04});
    g_map.setSlot(Action::Mode,  ResistiveMap::Slot{0, 0x02});
    g_sink.learnAction(Action::EncCw, 4000, 1000);
    resetLog();

    g_sink.onKey(ev(Action::Mode,  State::Press, 1500));
    g_sink.onKey(ev(Action::EncCw, State::Press, 1600));
    g_sink.tick(2000);
    // The unit is mid-measurement. A stray press changing the voltage now is
    // how a study page confidently learns the wrong button.
    TEST_ASSERT_EQUAL_UINT8(0, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x04, g_sink.liveMask(0));
}

static void test_learning_an_unassigned_action_fails_rather_than_asserting_nothing(void)
{
    freshSink();
    TEST_ASSERT_FALSE(g_sink.learnAction(Action::Voice, 4000, 1000));
    TEST_ASSERT_EQUAL_UINT8(0x00, g_sink.liveMask(0));
}

static void test_learn_cancel_breaks_the_hold_immediately(void)
{
    freshSink();
    g_map.setSlot(Action::Mode, ResistiveMap::Slot{0, 0x02});
    g_sink.learnAction(Action::Mode, 4000, 1000);
    g_sink.learnCancel(1500);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_sink.liveMask(0));
    TEST_ASSERT_FALSE(g_sink.learning(0));
}

// ---------------------------------------------------------------------------
// The long rung — nine keys becoming eighteen
// ---------------------------------------------------------------------------
static void test_a_button_without_a_long_rung_is_not_delayed(void)
{
    freshSink();
    g_map.setSlot(Action::VolUp, ResistiveMap::Slot{0, 0x01, 0x00});
    g_sink.onKey(ev(Action::VolUp, State::Press, 1000));
    // THE COST OF THE FEATURE MUST NOT BE PAID BY BUTTONS THAT DECLINED IT.
    // Volume acts on contact and repeats while held; deferring it by the
    // long-press threshold would be the whole feature going wrong at once.
    TEST_ASSERT_EQUAL_UINT8(1, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x01, g_log[0].mask);
}

static void test_a_short_press_of_a_dual_button_arrives_on_release(void)
{
    freshSink();
    g_map.setSlot(Action::Mode, ResistiveMap::Slot{0, 0x02, 0x04});

    g_sink.onKey(ev(Action::Mode, State::Press, 1000));
    // Nothing yet: a resistance cannot be un-pressed, so the sink has to know
    // which of the two rungs this is before it commits.
    TEST_ASSERT_EQUAL_UINT8(0, g_n);

    g_sink.onKey(ev(Action::Mode, State::Release, 1200));
    TEST_ASSERT_EQUAL_UINT8(1, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x02, g_log[0].mask);        // the SHORT rung

    // ...and it releases itself, because the finger is already off.
    g_sink.tick(1200 + g_map.timing().pulseMs + 1);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_sink.liveMask(0));
}

static void test_a_long_press_of_a_dual_button_takes_the_other_rung(void)
{
    freshSink();
    g_map.setSlot(Action::Mode, ResistiveMap::Slot{0, 0x02, 0x04});

    g_sink.onKey(ev(Action::Mode, State::Press, 1000));
    g_sink.onKey(ev(Action::Mode, State::Long,  1600));
    TEST_ASSERT_EQUAL_UINT8(1, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x04, g_log[0].mask);        // the LONG rung
    TEST_ASSERT_EQUAL_UINT32(1, g_sink.longs());

    // Held, not pulsed — the head unit sees a second distinct button down.
    g_sink.onKey(ev(Action::Mode, State::Repeat, 1750));
    TEST_ASSERT_EQUAL_UINT8(0x04, g_sink.liveMask(0));

    g_sink.onKey(ev(Action::Mode, State::Release, 2000));
    TEST_ASSERT_EQUAL_UINT8(0x00, g_sink.liveMask(0));
}

static void test_the_short_rung_never_fires_when_the_long_one_did(void)
{
    freshSink();
    g_map.setSlot(Action::Mode, ResistiveMap::Slot{0, 0x02, 0x04});

    g_sink.onKey(ev(Action::Mode, State::Press,   1000));
    g_sink.onKey(ev(Action::Mode, State::Long,    1600));
    g_sink.onKey(ev(Action::Mode, State::Release, 2000));
    // Two rungs for one press would be two buttons for one finger — the head
    // unit would act on both, which is the failure that makes a dual-function
    // key useless.
    for (uint8_t i = 0; i < g_n; ++i)
        TEST_ASSERT_NOT_EQUAL(0x02, g_log[i].mask);
}

static void test_a_long_rung_equal_to_the_short_one_is_refused(void)
{
    ResistiveMap m;
    m.loadDefaults();
    m.setSlot(Action::Mode, ResistiveMap::Slot{0, 0x02, 0x02});
    // Otherwise the button would simply become slower, with nothing to show
    // for it, and the settings page would report it as configured.
    TEST_ASSERT_EQUAL_UINT8(0, m.slot(Action::Mode).longMask);
}

static void test_a_wheel_detent_cannot_carry_a_long_rung(void)
{
    ResistiveMap a;
    a.loadDefaults();
    a.setSlot(Action::EncCw, ResistiveMap::Slot{0, 0x01, 0x08});
    uint8_t blob[ResistiveMap::BLOB_SIZE];
    a.toBlob(blob);
    ResistiveMap b;
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));
    // A detent has no hold, so this could never fire. Cleared rather than left
    // in the UI looking configured.
    TEST_ASSERT_EQUAL_UINT8(0, b.slot(Action::EncCw).longMask);
}

static void test_a_second_press_retires_a_deferred_one(void)
{
    freshSink();
    g_map.setSlot(Action::Mode,  ResistiveMap::Slot{0, 0x02, 0x04});   // dual
    g_map.setSlot(Action::Radio, ResistiveMap::Slot{0, 0x08, 0x00});   // plain

    g_sink.onKey(ev(Action::Mode,  State::Press, 1000));   // deferred, wire idle
    g_sink.onKey(ev(Action::Radio, State::Press, 1100));   // takes the channel
    TEST_ASSERT_EQUAL_UINT8(0x08, g_sink.liveMask(0));
    resetLog();

    g_sink.onKey(ev(Action::Mode, State::Release, 1200));
    // Mode was overtaken. Its Release must NOT put the short rung on top of the
    // rung Radio is holding — which would also strand Radio, because its own
    // Release would then find an owner that no longer matches.
    TEST_ASSERT_EQUAL_UINT8(0, g_n);
    TEST_ASSERT_EQUAL_UINT8(0x08, g_sink.liveMask(0));

    g_sink.onKey(ev(Action::Radio, State::Release, 1400));
    TEST_ASSERT_EQUAL_UINT8(0x00, g_sink.liveMask(0));     // and it still lifts
}

static void test_the_long_rung_survives_a_blob_round_trip(void)
{
    ResistiveMap a;
    a.loadDefaults();
    a.setSlot(Action::Mode, ResistiveMap::Slot{1, 0x02, 0x10});
    uint8_t blob[ResistiveMap::BLOB_SIZE];
    a.toBlob(blob);
    ResistiveMap b;
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT8(0x10, b.slot(Action::Mode).longMask);
    TEST_ASSERT_EQUAL_UINT8(1,    b.slot(Action::Mode).ch);
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_a_single_leg_is_its_own_resistor_plus_the_series_term);
    RUN_TEST(test_two_legs_are_in_parallel);
    RUN_TEST(test_no_leg_engaged_is_open_and_not_zero_ohms);
    RUN_TEST(test_a_mask_pointing_at_unfitted_legs_reads_open);
    RUN_TEST(test_a_leg_below_the_floor_is_clamped_not_ignored);

    RUN_TEST(test_four_buttons_on_four_legs_get_one_leg_each);
    RUN_TEST(test_one_leg_per_button_is_far_better_separated_than_combinations);
    RUN_TEST(test_more_buttons_than_legs_falls_back_to_combinations);
    RUN_TEST(test_a_rung_outside_the_window_is_never_handed_out);
    RUN_TEST(test_the_spread_maximises_the_worst_ratio_not_the_linear_gap);
    RUN_TEST(test_two_actions_on_one_rung_report_zero_separation);

    RUN_TEST(test_nothing_is_assigned_by_default);
    RUN_TEST(test_an_encoder_phase_can_never_be_given_a_rung);
    RUN_TEST(test_the_map_survives_a_blob_round_trip);
    RUN_TEST(test_a_blob_of_the_wrong_length_is_rejected_whole);
    RUN_TEST(test_a_stored_zero_hold_guard_is_repaired_on_load);
    RUN_TEST(test_auto_assign_is_idempotent);

    RUN_TEST(test_a_press_engages_and_a_release_opens);
    RUN_TEST(test_long_and_repeat_do_not_touch_the_line);
    RUN_TEST(test_an_unassigned_action_is_silent);
    RUN_TEST(test_a_rung_on_unfitted_legs_says_so_rather_than_acting);
    RUN_TEST(test_routing_off_means_silent);
    RUN_TEST(test_disabling_the_sink_releases_a_held_key);
    RUN_TEST(test_a_lost_release_is_broken_by_the_stuck_guard);

    RUN_TEST(test_two_rungs_are_never_engaged_at_once);
    RUN_TEST(test_a_release_from_a_preempted_key_does_not_lift_the_new_one);
    RUN_TEST(test_the_two_channels_are_independent);

    RUN_TEST(test_a_detent_pulses_and_releases_itself);
    RUN_TEST(test_the_gap_is_enforced_between_two_detents);
    RUN_TEST(test_reversing_the_wheel_drops_the_pending_steps);
    RUN_TEST(test_the_queue_has_a_ceiling_and_counts_what_it_drops);
    RUN_TEST(test_a_button_press_preempts_a_spinning_wheel);
    RUN_TEST(test_hybrid_holds_only_once_the_queue_saturates);
    RUN_TEST(test_a_hybrid_hold_lasts_the_dead_time_plus_one_period_per_detent);

    RUN_TEST(test_the_default_ladder_is_inside_the_units_usable_window);
    RUN_TEST(test_a_leg_above_the_window_is_flagged_not_silently_accepted);
    RUN_TEST(test_an_encoder_direction_can_be_taught_which_a_detent_never_could);
    RUN_TEST(test_a_learn_hold_ignores_routing_so_setup_is_not_circular);
    RUN_TEST(test_nothing_disturbs_a_learn_hold);
    RUN_TEST(test_learning_an_unassigned_action_fails_rather_than_asserting_nothing);
    RUN_TEST(test_learn_cancel_breaks_the_hold_immediately);

    RUN_TEST(test_a_button_without_a_long_rung_is_not_delayed);
    RUN_TEST(test_a_short_press_of_a_dual_button_arrives_on_release);
    RUN_TEST(test_a_long_press_of_a_dual_button_takes_the_other_rung);
    RUN_TEST(test_the_short_rung_never_fires_when_the_long_one_did);
    RUN_TEST(test_a_long_rung_equal_to_the_short_one_is_refused);
    RUN_TEST(test_a_second_press_retires_a_deferred_one);
    RUN_TEST(test_a_wheel_detent_cannot_carry_a_long_rung);
    RUN_TEST(test_the_long_rung_survives_a_blob_round_trip);

    return UNITY_END();
}
