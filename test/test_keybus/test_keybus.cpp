// The KeyBus policy: which key goes first, and what happens to a key the bus
// could not hold (docs/design/event-tasks.md §3.2, §3.4).
//
// Every failure here is felt from the driver's seat and invisible on a bench:
//
//   * a RELEASE handled before its own press. The release goes out first, the
//     press after it, and the head unit holds the key down for ever.
//   * a TAP split in two. A knob click is one item precisely so that a full
//     queue drops all of it or none of it.
//   * a LOST RELEASE. A full queue may lose a press (a missed key) but must
//     never lose a release silently (a stuck key): the keys task lets it go.
//   * a release scheduled in the past. A detent that waited in the queue would
//     have its release due already, and it would leave on the heels of its
//     press, which is the case some head units drop.
#include <unity.h>
#include <string.h>

#include "keys/KeyRouter.h"
#include "keys/KeyEngine.h"
#include "keys/IKeySink.h"
#include "sinks/CanboxUartSink.h"
#include "proto/canbox/Rav4Profile.h"

using namespace Keys;

void setUp() {}
void tearDown() {}

// ---- harness ---------------------------------------------------------------

static Event mk(Action a, State s, uint8_t src = 0, uint32_t t = 0)
{
    Event e;
    e.action = a; e.state = s; e.srcId = src; e.tMs = t;
    return e;
}

static KeyMsg msgOf(const Event& e, KeyMsg::Kind k = KeyMsg::Kind::Event)
{
    KeyMsg m;
    m.kind = k;
    m.ev = e;
    return m;
}

// What reached dispatch, in order.
static Event   g_out[64];
static uint8_t g_outN = 0;
static void onDispatch(const Event& e, void* ctx)
{
    if (g_outN < 64) g_out[g_outN++] = e;
    // The engine's event hook feeds the down set; the harness does the same.
    if (ctx) static_cast<KeyRouter*>(ctx)->observe(e);
}

static KeyRouter router()
{
    g_outN = 0;
    KeyRouter r;
    return r;
}

static void wire(KeyRouter& r)
{
    KeyRouter::Hooks h;
    h.dispatch = onDispatch;
    h.ctx = &r;
    r.begin(h);
}

// Drain the queues through the router, as the keys task does.
static void drain(KeyQueues& q, KeyRouter& r)
{
    KeyMsg m;
    while (q.pop(m)) r.handle(m);
}

// ---- order -----------------------------------------------------------------

static void test_a_release_goes_before_waiting_detents()
{
    KeyQueues q;
    q.push(Lane::Tap, msgOf(mk(Action::EncCw, State::Press, 4)));
    q.push(Lane::Tap, msgOf(mk(Action::EncCw, State::Press, 4)));
    q.push(Lane::Rel, msgOf(mk(Action::VolUp, State::Release, 0)));
    KeyMsg m;
    TEST_ASSERT_TRUE(q.pop(m));
    TEST_ASSERT_EQUAL((int)Action::VolUp, (int)m.ev.action);
    TEST_ASSERT_EQUAL((int)State::Release, (int)m.ev.state);
}

static void test_a_release_never_overtakes_its_own_press()
{
    KeyQueues q;
    q.push(Lane::Tap, msgOf(mk(Action::EncCw, State::Press, 4)));
    q.push(Lane::Tap, msgOf(mk(Action::VolUp, State::Press, 0)));
    q.push(Lane::Tap, msgOf(mk(Action::EncCcw, State::Press, 4)));
    q.push(Lane::Rel, msgOf(mk(Action::VolUp, State::Release, 0)));
    KeyMsg m;
    // Everything up to VOL+'s press, in arrival order; then its release; then the rest.
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::EncCw, (int)m.ev.action);
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::VolUp, (int)m.ev.action);
    TEST_ASSERT_EQUAL((int)State::Press, (int)m.ev.state);
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::VolUp, (int)m.ev.action);
    TEST_ASSERT_EQUAL((int)State::Release, (int)m.ev.state);
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::EncCcw, (int)m.ev.action);
    TEST_ASSERT_FALSE(q.pop(m));
}

// VOL+ down, VOL- down, VOL+ up, all queued before the keys task runs: a
// combo macro on VOL+ & VOL- must still see both keys down at once.
static void test_a_release_waits_for_an_earlier_press_of_another_key()
{
    KeyQueues q;
    q.push(Lane::Tap, msgOf(mk(Action::VolUp, State::Press, 0)));
    q.push(Lane::Tap, msgOf(mk(Action::VolDown, State::Press, 0)));
    q.push(Lane::Rel, msgOf(mk(Action::VolUp, State::Release, 0)));
    q.push(Lane::Tap, msgOf(mk(Action::Ok, State::Press, 0)));
    KeyMsg m;
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::VolUp, (int)m.ev.action);
    TEST_ASSERT_EQUAL((int)State::Press, (int)m.ev.state);
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::VolDown, (int)m.ev.action);
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::VolUp, (int)m.ev.action);
    TEST_ASSERT_EQUAL((int)State::Release, (int)m.ev.state);
    // A press that came after the release still waits for it.
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::Ok, (int)m.ev.action);
    TEST_ASSERT_FALSE(q.pop(m));
}

// A detent queued before a release of another key, from any origin, is passed.
static void test_a_release_passes_detents_but_not_a_knob_press()
{
    KeyQueues q;
    q.push(Lane::Tap, msgOf(mk(Action::EncCw, State::Press, 4)));                 // knob detent
    q.push(Lane::Tap, msgOf(mk(Action::Ok, State::Press, 4), KeyMsg::Kind::Tap)); // knob click
    q.push(Lane::Tap, msgOf(mk(Action::EncCcw, State::Press, 4)));                // knob detent
    q.push(Lane::Rel, msgOf(mk(Action::VolUp, State::Release, 0)));               // the matrix
    KeyMsg m;
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::EncCw, (int)m.ev.action);
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::Ok, (int)m.ev.action);
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::VolUp, (int)m.ev.action);
    TEST_ASSERT_EQUAL((int)State::Release, (int)m.ev.state);
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::EncCcw, (int)m.ev.action);
    TEST_ASSERT_FALSE(q.pop(m));
}

static void test_the_web_waits_for_every_key()
{
    KeyQueues q;
    q.push(Lane::Web, msgOf(mk(Action::Mute, State::Press, 2), KeyMsg::Kind::Tap));
    q.push(Lane::Tap, msgOf(mk(Action::EncCw, State::Press, 4)));
    q.push(Lane::Rel, msgOf(mk(Action::Ok, State::Release, 0)));
    KeyMsg m;
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::Ok, (int)m.ev.action);
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::EncCw, (int)m.ev.action);
    TEST_ASSERT_TRUE(q.pop(m)); TEST_ASSERT_EQUAL((int)Action::Mute, (int)m.ev.action);
    TEST_ASSERT_TRUE(q.empty());
}

static void test_web_spam_is_dropped_and_counted_and_never_reaches_the_key_lanes()
{
    KeyQueues q;
    uint8_t ok = 0;
    for (uint8_t i = 0; i < 10; ++i)
        if (q.push(Lane::Web, msgOf(mk(Action::Mute, State::Press, 2), KeyMsg::Kind::Tap))) ++ok;
    TEST_ASSERT_EQUAL(KeyQueues::WEB_DEPTH, ok);
    TEST_ASSERT_EQUAL(10 - KeyQueues::WEB_DEPTH, q.drops(Lane::Web));
    // A key still gets in, and still goes first.
    TEST_ASSERT_TRUE(q.push(Lane::Tap, msgOf(mk(Action::EncCw, State::Press, 4))));
    KeyMsg m;
    TEST_ASSERT_TRUE(q.pop(m));
    TEST_ASSERT_EQUAL((int)Action::EncCw, (int)m.ev.action);
}

static void test_a_full_tap_lane_drops_and_counts_per_origin()
{
    KeyQueues q;
    for (uint8_t i = 0; i < KeyQueues::TAP_DEPTH; ++i)
        TEST_ASSERT_TRUE(q.push(Lane::Tap, msgOf(mk(Action::EncCw, State::Press, 4))));
    TEST_ASSERT_FALSE(q.push(Lane::Tap, msgOf(mk(Action::EncCw, State::Press, 4))));
    TEST_ASSERT_FALSE(q.push(Lane::Tap, msgOf(mk(Action::EncCw, State::Press, 0))));
    TEST_ASSERT_EQUAL(2, q.drops(Lane::Tap));
    TEST_ASSERT_EQUAL(1, q.tapDropsFrom(4));
    TEST_ASSERT_EQUAL(1, q.tapDropsFrom(0));
}

// ---- the Tap ---------------------------------------------------------------

static void test_a_tap_is_one_item_and_becomes_press_then_release()
{
    KeyQueues q;
    KeyRouter r = router(); wire(r);
    q.push(Lane::Tap, msgOf(mk(Action::Ok, State::Press, 4), KeyMsg::Kind::Tap));
    TEST_ASSERT_EQUAL(1, q.size(Lane::Tap));
    drain(q, r);
    TEST_ASSERT_EQUAL(2, g_outN);
    TEST_ASSERT_EQUAL((int)State::Press,   (int)g_out[0].state);
    TEST_ASSERT_EQUAL((int)State::Release, (int)g_out[1].state);
    TEST_ASSERT_EQUAL(0, r.down().size());
}

static void test_a_tap_that_does_not_fit_is_dropped_whole()
{
    KeyQueues q;
    for (uint8_t i = 0; i < KeyQueues::TAP_DEPTH; ++i)
        q.push(Lane::Tap, msgOf(mk(Action::EncCw, State::Press, 4)));
    TEST_ASSERT_FALSE(q.push(Lane::Tap, msgOf(mk(Action::Ok, State::Press, 4), KeyMsg::Kind::Tap)));
    KeyRouter r = router(); wire(r);
    drain(q, r);
    // Only the detents: no press of OK without its release, and no release alone.
    for (uint8_t i = 0; i < g_outN; ++i) TEST_ASSERT_EQUAL((int)Action::EncCw, (int)g_out[i].action);
}

static void test_a_detent_tap_has_no_release()
{
    KeyQueues q;
    KeyRouter r = router(); wire(r);
    q.push(Lane::Web, msgOf(mk(Action::EncCw, State::Press, 2), KeyMsg::Kind::Tap));
    drain(q, r);
    TEST_ASSERT_EQUAL(1, g_outN);
    TEST_ASSERT_EQUAL((int)State::Press, (int)g_out[0].state);
}

// ---- the stuck key ---------------------------------------------------------

static void test_a_lost_release_is_synthesised_for_a_key_still_down()
{
    KeyQueues q;
    KeyRouter r = router(); wire(r);
    q.push(Lane::Tap, msgOf(mk(Action::VolUp, State::Press, 0, 100)));
    drain(q, r);
    TEST_ASSERT_TRUE(r.down().has(0, Action::VolUp));

    // Fill the Rel lane with other keys' releases, then lose VOL+'s.
    for (uint8_t i = 0; i < KeyQueues::REL_DEPTH; ++i)
        q.push(Lane::Rel, msgOf(mk(Action::Mute, State::Release, 1)));
    TEST_ASSERT_FALSE(q.push(Lane::Rel, msgOf(mk(Action::VolUp, State::Release, 0))));
    TEST_ASSERT_EQUAL(1, q.drops(Lane::Rel));

    drain(q, r);
    Event lost[KeyQueues::LOST_MAX];
    const uint8_t n = q.takeLost(lost, KeyQueues::LOST_MAX);
    TEST_ASSERT_EQUAL(1, n);
    g_outN = 0;
    TEST_ASSERT_EQUAL(1, r.synthesise(lost, n, 500));
    TEST_ASSERT_EQUAL(1, g_outN);
    TEST_ASSERT_EQUAL((int)Action::VolUp, (int)g_out[0].action);
    TEST_ASSERT_EQUAL((int)State::Release, (int)g_out[0].state);
    TEST_ASSERT_EQUAL(500u, g_out[0].tMs);
    TEST_ASSERT_EQUAL(1, r.stuckSynth());
    TEST_ASSERT_FALSE(r.down().has(0, Action::VolUp));
    // Taken once.
    TEST_ASSERT_EQUAL(0, q.takeLost(lost, KeyQueues::LOST_MAX));
}

static void test_a_lost_release_of_a_key_that_is_not_down_sends_nothing()
{
    KeyRouter r = router(); wire(r);
    Event lost[1] = { mk(Action::VolUp, State::Release, 0) };
    TEST_ASSERT_EQUAL(0, r.synthesise(lost, 1, 10));
    TEST_ASSERT_EQUAL(0, g_outN);
    TEST_ASSERT_EQUAL(0, r.stuckSynth());
}

static void test_a_held_key_with_its_release_queued_is_never_synthesised()
{
    // Holding a key is normal use: the down set alone never releases anything.
    KeyQueues q;
    KeyRouter r = router(); wire(r);
    q.push(Lane::Tap, msgOf(mk(Action::Phone, State::Press, 0, 0)));
    q.push(Lane::Tap, msgOf(mk(Action::Phone, State::Long, 0, 600)));
    drain(q, r);
    Event lost[KeyQueues::LOST_MAX];
    TEST_ASSERT_EQUAL(0, r.synthesise(lost, q.takeLost(lost, KeyQueues::LOST_MAX), 60000));
    TEST_ASSERT_TRUE(r.down().has(0, Action::Phone));
    TEST_ASSERT_EQUAL(0, r.stuckSynth());
}

// A knob spin keeping the Tap lane busy must not hold back a lost release:
// only items older than it count.
static void test_a_lost_release_is_ready_once_everything_older_is_handled()
{
    KeyQueues q;
    KeyRouter r = router(); wire(r);
    q.push(Lane::Tap, msgOf(mk(Action::VolUp, State::Press, 0, 100)));
    for (uint8_t i = 0; i < KeyQueues::REL_DEPTH; ++i)
        q.push(Lane::Rel, msgOf(mk(Action::Mute, State::Release, 1)));
    TEST_ASSERT_FALSE(q.push(Lane::Rel, msgOf(mk(Action::VolUp, State::Release, 0))));
    // Newer traffic behind the lost release.
    q.push(Lane::Tap, msgOf(mk(Action::EncCw, State::Press, 4)));

    Event lost[KeyQueues::LOST_MAX];
    bool all = true;
    TEST_ASSERT_EQUAL(0, q.takeLostReady(lost, KeyQueues::LOST_MAX, all));   // its press is still queued
    TEST_ASSERT_FALSE(all);

    // Handle everything up to the lost release; the detent after it stays.
    KeyMsg m;
    for (uint8_t i = 0; i < 1 + KeyQueues::REL_DEPTH; ++i) { TEST_ASSERT_TRUE(q.pop(m)); r.handle(m); }
    TEST_ASSERT_EQUAL(1, q.size(Lane::Tap));
    TEST_ASSERT_EQUAL(1, q.takeLostReady(lost, KeyQueues::LOST_MAX, all));
    TEST_ASSERT_EQUAL((int)Action::VolUp, (int)lost[0].action);
    TEST_ASSERT_EQUAL(0, q.takeLostReady(lost, KeyQueues::LOST_MAX, all));   // taken once
}

// More lost releases than LOST_MAX can name: every key still down is let go,
// with the code its press carried.
static void test_lost_releases_past_the_list_release_every_key_down()
{
    KeyQueues q;
    KeyRouter r = router(); wire(r);
    Event p = mk(Action::Phone, State::Press, 0, 0);
    p.codeHint = 0x42;
    q.push(Lane::Tap, msgOf(p));
    drain(q, r);
    TEST_ASSERT_TRUE(r.down().has(0, Action::Phone));

    for (uint8_t i = 0; i < KeyQueues::REL_DEPTH; ++i)
        q.push(Lane::Rel, msgOf(mk(Action::Mute, State::Release, 1)));
    for (uint8_t i = 0; i < KeyQueues::LOST_MAX + 1; ++i)
        q.push(Lane::Rel, msgOf(mk(Action::VolDown, State::Release, 2)));
    TEST_ASSERT_EQUAL(1u, q.lostOverflow());
    drain(q, r);

    Event lost[KeyQueues::LOST_MAX];
    bool all = false;
    TEST_ASSERT_EQUAL(KeyQueues::LOST_MAX, q.takeLostReady(lost, KeyQueues::LOST_MAX, all));
    TEST_ASSERT_TRUE(all);
    g_outN = 0;
    TEST_ASSERT_EQUAL(1, r.releaseAll(900));
    TEST_ASSERT_EQUAL(1, g_outN);
    TEST_ASSERT_EQUAL((int)Action::Phone, (int)g_out[0].action);
    TEST_ASSERT_EQUAL((int)State::Release, (int)g_out[0].state);
    TEST_ASSERT_EQUAL_UINT8(0x42, g_out[0].codeHint);
    TEST_ASSERT_EQUAL(0, r.down().size());
    TEST_ASSERT_EQUAL(0, q.takeLostReady(lost, KeyQueues::LOST_MAX, all));
    TEST_ASSERT_FALSE(all);
}

static void test_detents_are_never_down()
{
    DownSet d;
    d.note(mk(Action::EncCw, State::Press, 4));
    TEST_ASSERT_EQUAL(0, d.size());
}

// ---- the other message kinds ------------------------------------------------

static uint16_t g_panelCode = 0; static bool g_panelHold = false; static uint32_t g_panelT = 0;
static uint8_t  g_litCode = 0, g_litState = 0; static bool g_litRel = false;
static int      g_macro = -1;

static void test_panel_literal_and_macro_messages_reach_their_hooks()
{
    KeyRouter r = router();
    KeyRouter::Hooks h;
    h.panelKey = [](uint16_t c, bool hold, uint32_t t, void*) { g_panelCode = c; g_panelHold = hold; g_panelT = t; };
    h.literal  = [](uint8_t c, uint8_t s, bool rel, void*) { g_litCode = c; g_litState = s; g_litRel = rel; };
    h.macroRun = [](uint8_t i, void*) { g_macro = i; };
    r.begin(h);

    KeyMsg m;
    m.kind = KeyMsg::Kind::PanelKey; m.code = 0x0113; m.a = 1; m.ev.tMs = 1234;
    r.handle(m);
    TEST_ASSERT_EQUAL_HEX16(0x0113, g_panelCode);
    TEST_ASSERT_TRUE(g_panelHold);
    TEST_ASSERT_EQUAL(1234u, g_panelT);

    m = KeyMsg{}; m.kind = KeyMsg::Kind::Literal; m.code = 0x07; m.a = 1; m.b = 1;
    r.handle(m);
    TEST_ASSERT_EQUAL_HEX8(0x07, g_litCode);
    TEST_ASSERT_EQUAL(1, g_litState);
    TEST_ASSERT_TRUE(g_litRel);

    m = KeyMsg{}; m.kind = KeyMsg::Kind::MacroRun; m.a = 5;
    r.handle(m);
    TEST_ASSERT_EQUAL(5, g_macro);
}

// ---- the release deadline ----------------------------------------------------

static void test_a_fresh_key_releases_a_pulse_after_its_source_time()
{
    TEST_ASSERT_EQUAL(160u, releaseDue(100, 60, 101));
}

static void test_a_queued_key_never_gets_a_release_sooner_than_the_minimum_gap()
{
    // It waited 55 ms: tMs + pulse is 5 ms away, so the gap wins.
    TEST_ASSERT_EQUAL(155u + MIN_GAP_MS, releaseDue(100, 60, 155));
    // Already past due.
    TEST_ASSERT_EQUAL(400u + MIN_GAP_MS, releaseDue(100, 60, 400));
}

static void test_the_deadline_rule_survives_the_millis_wrap()
{
    const uint32_t t = 0xFFFFFFF0u;
    TEST_ASSERT_EQUAL((uint32_t)(t + 60), releaseDue(t, 60, t + 1));
    TEST_ASSERT_EQUAL((uint32_t)(t + 100 + MIN_GAP_MS), releaseDue(t, 60, t + 100));
}

// The UART sink applies the rule to a non-knob wheel code.
static uint8_t  g_frames[8][8];
static uint8_t  g_frameN = 0;
static uint32_t g_clock = 0;
static void capture(const uint8_t* f, uint8_t len, void*)
{
    if (g_frameN < 8) { memcpy(g_frames[g_frameN], f, len < 8 ? len : 8); ++g_frameN; }
}

static void test_the_uart_sink_schedules_a_queued_detents_release_from_now()
{
    Sinks::CanboxUartSink s;
    g_frameN = 0;
    s.begin(capture, nullptr);
    s.setClock([]() -> uint32_t { return g_clock; });
    s.setPulseMs(60);

    Event e = mk(Action::EncCw, State::Press, 3, 1000);
    e.codeHint = Canbox::RAV4_UP;                        // a code with a press/release form

    g_clock = 1070;                                      // it waited 70 ms on the bus
    s.onKey(e);
    TEST_ASSERT_EQUAL(1, g_frameN);
    uint32_t due = 0;
    TEST_ASSERT_TRUE(s.nextDue(due));
    TEST_ASSERT_EQUAL(1070u + MIN_GAP_MS, due);
    s.tick(1070 + MIN_GAP_MS - 1);
    TEST_ASSERT_EQUAL(1, g_frameN);
    s.tick(1070 + MIN_GAP_MS);
    TEST_ASSERT_EQUAL(2, g_frameN);
    TEST_ASSERT_FALSE(s.nextDue(due));
}

// ---- the sink chain ----------------------------------------------------------

struct Rec : ISink {
    const char* name;
    static char order[16];
    static uint8_t n;
    explicit Rec(const char* nm) : name(nm) {}
    void onKey(const Event&) override { if (n < sizeof(order) - 1) order[n++] = name[0]; order[n] = 0; }
    const char* id() const override { return name; }
};
char Rec::order[16] = {};
uint8_t Rec::n = 0;

static void test_the_uart_sink_registered_first_hears_every_key_first()
{
    KeyEngine eng;
    Rec u("uart"), a("ams"), h("hid");
    TEST_ASSERT_TRUE(eng.addSink(&u));
    TEST_ASSERT_TRUE(eng.addSink(&a));
    TEST_ASSERT_TRUE(eng.addSink(&h));
    Rec::n = 0;
    eng.dispatch(mk(Action::Ok, State::Press));
    TEST_ASSERT_EQUAL_STRING("uah", Rec::order);
}

static void test_a_seventh_sink_is_refused_and_the_six_still_work()
{
    KeyEngine eng;
    Rec s0("0"), s1("1"), s2("2"), s3("3"), s4("4"), s5("5"), s6("6");
    ISink* all[] = { &s0, &s1, &s2, &s3, &s4, &s5 };
    for (ISink* s : all) TEST_ASSERT_TRUE(eng.addSink(s));
    TEST_ASSERT_FALSE(eng.addSink(&s6));
    Rec::n = 0;
    eng.dispatch(mk(Action::Ok, State::Press));
    TEST_ASSERT_EQUAL_STRING("012345", Rec::order);
}

// The matrix half hands its events to the bus instead of the sinks.
static Event   g_emitted[4];
static uint8_t g_emitN = 0;

static void test_with_an_emit_hook_dispatch_is_not_called_from_poll()
{
    KeyEngine eng;
    Rec u("uart");
    eng.addSink(&u);
    eng.onEmit([](const Event& e, void*) { if (g_emitN < 4) g_emitted[g_emitN++] = e; }, nullptr);
    // No scanner: poll() returns at once. What matters is the routing of
    // dispatch(): injected events still reach the sinks directly.
    Rec::n = 0;
    eng.poll(0);
    TEST_ASSERT_EQUAL(0, g_emitN);
    eng.dispatch(mk(Action::Ok, State::Press));
    TEST_ASSERT_EQUAL(1, Rec::n);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_release_goes_before_waiting_detents);
    RUN_TEST(test_a_release_never_overtakes_its_own_press);
    RUN_TEST(test_a_release_waits_for_an_earlier_press_of_another_key);
    RUN_TEST(test_a_release_passes_detents_but_not_a_knob_press);
    RUN_TEST(test_the_web_waits_for_every_key);
    RUN_TEST(test_web_spam_is_dropped_and_counted_and_never_reaches_the_key_lanes);
    RUN_TEST(test_a_full_tap_lane_drops_and_counts_per_origin);
    RUN_TEST(test_a_tap_is_one_item_and_becomes_press_then_release);
    RUN_TEST(test_a_tap_that_does_not_fit_is_dropped_whole);
    RUN_TEST(test_a_detent_tap_has_no_release);
    RUN_TEST(test_a_lost_release_is_synthesised_for_a_key_still_down);
    RUN_TEST(test_a_lost_release_of_a_key_that_is_not_down_sends_nothing);
    RUN_TEST(test_a_held_key_with_its_release_queued_is_never_synthesised);
    RUN_TEST(test_a_lost_release_is_ready_once_everything_older_is_handled);
    RUN_TEST(test_lost_releases_past_the_list_release_every_key_down);
    RUN_TEST(test_detents_are_never_down);
    RUN_TEST(test_panel_literal_and_macro_messages_reach_their_hooks);
    RUN_TEST(test_a_fresh_key_releases_a_pulse_after_its_source_time);
    RUN_TEST(test_a_queued_key_never_gets_a_release_sooner_than_the_minimum_gap);
    RUN_TEST(test_the_deadline_rule_survives_the_millis_wrap);
    RUN_TEST(test_the_uart_sink_schedules_a_queued_detents_release_from_now);
    RUN_TEST(test_the_uart_sink_registered_first_hears_every_key_first);
    RUN_TEST(test_a_seventh_sink_is_refused_and_the_six_still_work);
    RUN_TEST(test_with_an_emit_hook_dispatch_is_not_called_from_poll);
    return UNITY_END();
}
