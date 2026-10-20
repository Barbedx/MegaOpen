#include <unity.h>
#include <string.h>
#include "input/QuadDecoder.h"
#include "input/PotStepper.h"
#include "knobs/KnobConfig.h"
#include "knobs/KnobSource.h"
#include "knobs/KnobLights.h"

using namespace Knobs;

void setUp() {}
void tearDown() {}

// ---- quadrature -------------------------------------------------------------
// One clockwise click of an EC11, A leading B: 00 -> 10 -> 11 -> 01 -> 00.
static const bool CW_A[4] = {true, true, false, false};
static const bool CW_B[4] = {false, true, true, false};

static int8_t clickCw(Input::QuadDecoder& q, uint32_t& t)
{
    int8_t sum = 0;
    for (uint8_t i = 0; i < 4; ++i) sum += q.update(CW_A[i], CW_B[i], ++t);
    return sum;
}
static int8_t clickCcw(Input::QuadDecoder& q, uint32_t& t)
{
    // The same four states, walked backwards: 00 -> 01 -> 11 -> 10 -> 00.
    static const bool A[4] = {false, true, true, false};
    static const bool B[4] = {true, true, false, false};
    int8_t sum = 0;
    for (uint8_t i = 0; i < 4; ++i) sum += q.update(A[i], B[i], ++t);
    return sum;
}

void test_quad_one_click_is_one_step()
{
    Input::QuadDecoder q; uint32_t t = 0;
    q.update(false, false, t);                      // seed at rest
    TEST_ASSERT_EQUAL_INT8(+1, clickCw(q, t));
    TEST_ASSERT_EQUAL_INT8(+1, clickCw(q, t));
    TEST_ASSERT_EQUAL_INT8(-1, clickCcw(q, t));
    TEST_ASSERT_EQUAL_UINT32(3, q.detents());
    TEST_ASSERT_EQUAL_UINT32(0, q.errors());
}

void test_quad_first_sample_is_a_position_not_a_step()
{
    // Powered up half way between two detents. That is a state, not a movement.
    Input::QuadDecoder q;
    TEST_ASSERT_EQUAL_INT8(0, q.update(true, true, 0));
    TEST_ASSERT_EQUAL_UINT32(0, q.errors());
}

void test_quad_bounce_cancels()
{
    Input::QuadDecoder q; uint32_t t = 0;
    q.update(false, false, t);
    // A chatters on its way closed. Every edge is a quarter step and its twin.
    for (uint8_t i = 0; i < 10; ++i) {
        TEST_ASSERT_EQUAL_INT8(0, q.update(true, false, ++t));
        TEST_ASSERT_EQUAL_INT8(0, q.update(false, false, ++t));
    }
    // ...and the click that follows is still exactly one.
    TEST_ASSERT_EQUAL_INT8(+1, clickCw(q, t));
}

void test_quad_inverted_swaps_direction()
{
    Input::QuadDecoder q; uint32_t t = 0;
    q.setInverted(true);
    q.update(false, false, t);
    TEST_ASSERT_EQUAL_INT8(-1, clickCw(q, t));
}

void test_quad_two_pulses_per_detent()
{
    Input::QuadDecoder q; uint32_t t = 0;
    q.setPulsesPerDetent(2);
    q.update(false, false, t);
    // Half-cycle part: 00 -> 10 -> 11 is already a click, and 11 -> 01 -> 00 the next.
    TEST_ASSERT_EQUAL_INT8(0,  q.update(true,  false, ++t));
    TEST_ASSERT_EQUAL_INT8(+1, q.update(true,  true,  ++t));
    TEST_ASSERT_EQUAL_INT8(0,  q.update(false, true,  ++t));
    TEST_ASSERT_EQUAL_INT8(+1, q.update(false, false, ++t));
}

void test_quad_double_jump_is_counted_not_guessed()
{
    // From rest there is no direction to go on: counted, not credited.
    Input::QuadDecoder q; uint32_t t = 0;
    q.update(false, false, t);
    TEST_ASSERT_EQUAL_INT8(0, q.update(true, true, ++t));    // both changed: which way?
    TEST_ASSERT_EQUAL_UINT32(1, q.errors());
}

void test_quad_missed_sample_mid_turn_keeps_the_click()
{
    // A late tick or a failed expander read while turning: 00 -> 10 -> (11
    // unseen) -> 01 -> 00 is still one whole click, on time, not half of one.
    Input::QuadDecoder q; uint32_t t = 0;
    q.update(false, false, t);
    TEST_ASSERT_EQUAL_INT8(0,  q.update(true,  false, ++t));  // +1
    TEST_ASSERT_EQUAL_INT8(0,  q.update(false, true,  ++t));  // jump: +2 the same way
    TEST_ASSERT_EQUAL_INT8(+1, q.update(false, false, ++t));  // +1 -> the detent
    TEST_ASSERT_EQUAL_UINT32(1, q.errors());
    TEST_ASSERT_EQUAL_UINT32(1, q.detents());

    // Counter-clockwise too, and a jump can complete the detent itself.
    TEST_ASSERT_EQUAL_INT8(0,  q.update(false, true,  ++t));  // -1
    TEST_ASSERT_EQUAL_INT8(0,  q.update(true,  true,  ++t));  // -1
    TEST_ASSERT_EQUAL_INT8(-1, q.update(false, false, ++t));  // jump: -2 -> the detent
    TEST_ASSERT_EQUAL_UINT32(2, q.errors());
    TEST_ASSERT_EQUAL_UINT32(2, q.detents());
    // ...and the next click is whole.
    TEST_ASSERT_EQUAL_INT8(-1, clickCcw(q, t));
}

void test_quad_rest_clears_a_missed_quarter()
{
    // THE ONE THIS CLASS HAS A TIMER FOR. Lose one quarter step and, without the
    // rest rule, every later click is reported half a detent late for ever.
    Input::QuadDecoder q; uint32_t t = 0;
    q.update(false, false, t);
    q.update(true, true, ++t);                      // missed 10 from rest: no direction, dropped
    q.update(false, true, ++t);                     // +1
    q.update(false, false, ++t);                    // +1  -> acc is 2 at a detent
    TEST_ASSERT_EQUAL_UINT32(0, q.detents());

    t += 500;
    q.update(false, false, t);                      // at rest: the stale half is forgotten
    TEST_ASSERT_EQUAL_INT8(+1, clickCw(q, t));      // a whole click is a whole click again
    TEST_ASSERT_EQUAL_INT8(-1, clickCcw(q, t));     // and reversing costs nothing
}

// ---- potentiometer -----------------------------------------------------------
static int feed(Input::PotStepper& p, uint16_t raw, uint8_t n = 40)
{
    int sum = 0;
    for (uint8_t i = 0; i < n; ++i) sum += p.update(raw);
    return sum;
}

void test_pot_first_reading_says_nothing()
{
    Input::PotStepper p;
    p.configure(Input::PotStepper::Params{});
    // Boots with the knob at three quarters. The head unit must not get 22 VolUps.
    TEST_ASSERT_EQUAL_INT8(0, p.update(3000));
    TEST_ASSERT_EQUAL_INT(0, feed(p, 3000));
    TEST_ASSERT_TRUE(p.zone() > 0);
}

void test_pot_full_sweep_is_zones_minus_one()
{
    Input::PotStepper p;
    Input::PotStepper::Params pr; pr.zones = 30;
    p.configure(pr);
    p.update(0);
    int up = 0;
    for (int v = 0; v <= 4095; v += 5) up += p.update((uint16_t)v);
    up += feed(p, 4095);
    TEST_ASSERT_EQUAL_INT(29, up);
    TEST_ASSERT_EQUAL_UINT8(29, p.zone());
    TEST_ASSERT_EQUAL_UINT8(255, p.level());

    int down = 0;
    for (int v = 4095; v >= 0; v -= 5) down += p.update((uint16_t)v);
    down += feed(p, 0);
    TEST_ASSERT_EQUAL_INT(-29, down);
    TEST_ASSERT_EQUAL_UINT8(0, p.level());
}

void test_pot_noise_on_a_boundary_does_not_chatter()
{
    Input::PotStepper p;
    Input::PotStepper::Params pr; pr.zones = 30;      // 136.5 counts per zone
    p.configure(pr);
    const uint16_t edge = 1365;                       // exactly zone 9 | 10
    p.update(edge);
    int sum = 0, moves = 0;
    for (int i = 0; i < 2000; ++i) {
        // +-25 counts of radio noise, deterministic.
        const int n = ((i * 37) % 51) - 25;
        const int8_t s = p.update((uint16_t)(edge + n));
        sum += s; if (s) ++moves;
    }
    // It may settle on either side of the line ONCE. It may not go back and forth.
    TEST_ASSERT_TRUE(moves <= 1);
    TEST_ASSERT_TRUE(sum >= -1 && sum <= 1);
}

void test_pot_inverted_flips_direction_not_calibration()
{
    Input::PotStepper p;
    Input::PotStepper::Params pr; pr.zones = 10; pr.inverted = true;
    pr.rawMin = 100; pr.rawMax = 3000;
    p.configure(pr);
    p.update(100);
    TEST_ASSERT_EQUAL_UINT8(255, p.level());          // the low reading is the TOP
    TEST_ASSERT_TRUE(feed(p, 3000, 80) < 0);          // counts rising = turning DOWN
    TEST_ASSERT_EQUAL_UINT8(0, p.level());
    TEST_ASSERT_EQUAL_UINT16(3000, p.raw());          // raw is never inverted
}

void test_pot_bad_calibration_falls_back_to_full_range()
{
    Input::PotStepper p;
    Input::PotStepper::Params pr; pr.rawMin = 2000; pr.rawMax = 2010;
    p.configure(pr);
    TEST_ASSERT_EQUAL_UINT16(0, p.params().rawMin);
    TEST_ASSERT_EQUAL_UINT16(4095, p.params().rawMax);
}

// ---- config ------------------------------------------------------------------
void test_config_defaults_are_silent()
{
    // The safety property, pinned: a freshly flashed board with nothing wired to
    // these pins must not be able to produce a key event or drive the LED pin.
    // (Native builds the DevKit profile. Behind a PCF8574, Board::KNOBS_I2C,
    // the encoders start on: those lines cannot float. KnobConfig.cpp.)
    Config c;
    TEST_ASSERT_FALSE(c.enc[ENC_RIGHT].enabled);
    TEST_ASSERT_FALSE(c.enc[ENC_LEFT].enabled);
    TEST_ASSERT_TRUE(c.potMode == PotMode::Off);
    TEST_ASSERT_FALSE(c.ledEnabled);
}

void test_config_blob_round_trip()
{
    Config a;
    a.enc[0].enabled = true; a.enc[0].inverted = true; a.enc[0].ppd = 2; a.enc[0].holdMs = 900;
    a.enc[1].enabled = true; a.enc[1].ppd = 1; a.enc[1].cw = Binding{Keys::Action::VolUp, 0x21}; a.enc[1].holdMs = 1200;
    a.enc[0].cw = Binding{Keys::Action::VolUp, 0x14}; a.enc[0].hold = Binding{Keys::Action::Mute, 0};
    a.potMode = PotMode::Steps; a.potInverted = true; a.potZones = 40;
    a.potMin = 123; a.potMax = 3456;
    a.ledEnabled = true; a.order = Order::RGB; a.brightness = 200;
    a.reactHoldMs = 0; a.levelSteps = 24; a.rightFirst = true; a.animPeriod = 25;
    a.zone[0].count = 3; a.zone[0].react = React::Flash; a.zone[0].anim = Anim::Rainbow;
    a.zone[0].led[0] = Rgb{1, 2, 3}; a.zone[0].led[7] = Rgb{10, 20, 30};   // first and LAST
    a.zone[1].count = 5; a.zone[1].react = React::Spin;
    a.zone[1].lo = Rgb{4, 5, 6}; a.zone[1].hi = Rgb{7, 8, 9};
    a.feetCount = 100; a.feetAnim = Anim::Breathe; a.feetColor = Rgb{11, 22, 33};
    a.feetBrightness = 77; a.maxMilliamps = 2500;

    uint8_t blob[Config::BLOB_SIZE];
    a.toBlob(blob);
    Config b;
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));

    uint8_t again[Config::BLOB_SIZE];
    b.toBlob(again);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob, again, sizeof(blob));
    TEST_ASSERT_TRUE(b.enc[0].cw.action == Keys::Action::VolUp);
    TEST_ASSERT_TRUE(b.enc[1].enabled);
    TEST_ASSERT_EQUAL_UINT8(1, b.enc[1].ppd);
    TEST_ASSERT_EQUAL_UINT8(0x21, b.enc[1].cw.code);
    TEST_ASSERT_EQUAL_UINT16(1200, b.enc[1].holdMs);
    TEST_ASSERT_EQUAL_UINT8(0x14, b.enc[0].cw.code);
    TEST_ASSERT_EQUAL_UINT16(3456, b.potMax);
    TEST_ASSERT_EQUAL_UINT8(9, b.zone[1].hi.b);
    TEST_ASSERT_TRUE(b.zone[0].anim == Anim::Rainbow);
    TEST_ASSERT_TRUE(b.zone[1].react == React::Spin);
    // The eighth colour of a zone showing three. It is not on the glass and it
    // still has to come back, or 3 -> 1 -> 3 on the page would lose it.
    TEST_ASSERT_EQUAL_UINT8(30, b.zone[0].led[7].b);
    TEST_ASSERT_EQUAL_UINT8(100, b.feetCount);
    TEST_ASSERT_TRUE(b.feetAnim == Anim::Breathe);
    TEST_ASSERT_EQUAL_UINT8(33, b.feetColor.b);
    TEST_ASSERT_EQUAL_UINT8(77, b.feetBrightness);
    TEST_ASSERT_EQUAL_UINT16(2500, b.maxMilliamps);
}

void test_config_supply_default_is_what_a_usb_port_can_give()
{
    // Pinned, because it is a safety default: LED current taken from a DevKit's
    // VIN goes through the board's 1 A Schottky.
    Config c;
    TEST_ASSERT_TRUE(c.maxMilliamps <= 500);
    TEST_ASSERT_EQUAL_UINT8(0, c.feetCount);
    c.maxMilliamps = 0; c.feetCount = 250;
    c.sanitize();
    TEST_ASSERT_TRUE(c.maxMilliamps >= 200);          // never so low that idle alone trips it
    TEST_ASSERT_EQUAL_UINT8(FEET_LEDS, c.feetCount);
}

void test_config_refuses_a_foreign_blob()
{
    uint8_t blob[Config::BLOB_SIZE];
    Config{}.toBlob(blob);
    Config c;
    blob[0] = (uint8_t)(Config::VERSION + 1);
    TEST_ASSERT_FALSE(c.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_FALSE(c.fromBlob(blob, (uint8_t)(sizeof(blob) - 1)));
}

void test_config_sanitize_strips_phases_and_caps_leds()
{
    Config c;
    c.enc[0].cw.action = Keys::Action::EncPhaseA;            // a matrix position, never an event
    c.zone[0].count = 12; c.zone[1].count = 3;
    c.sanitize();
    TEST_ASSERT_TRUE(c.enc[0].cw.action == Keys::Action::None);
    TEST_ASSERT_EQUAL_UINT8(ZONE_LEDS, c.zone[0].count);
    TEST_ASSERT_EQUAL_UINT8(3, c.zone[1].count);
}

// ---- source: movements -> key events -------------------------------------------
struct Tape {
    struct Item { Keys::Event e; bool pair; };
    Item    items[64];
    uint8_t n = 0;
    static void emit(const Keys::Event& e, bool pair, void* ctx) {
        auto* t = (Tape*)ctx;
        if (t->n < 64) t->items[t->n++] = Item{e, pair};
    }
};

static Config liveConfig()
{
    Config c;
    c.enc[0].enabled = true;
    c.enc[0].cw    = Binding{Keys::Action::VolUp, 0};
    c.enc[0].ccw   = Binding{Keys::Action::VolDown, 0};
    c.enc[0].click = Binding{Keys::Action::Mute, 0};
    c.potMode = PotMode::Steps;
    return c;
}

void test_source_disabled_emits_nothing()
{
    Tape t; Source s; s.begin(Tape::emit, &t);
    s.configure(Config{});                            // the defaults
    uint32_t now = 0;
    s.sampleEncoder(0, false, false, now);
    for (uint8_t i = 0; i < 4; ++i) s.sampleEncoder(0, CW_A[i], CW_B[i], ++now);
    for (uint8_t i = 0; i < 100; ++i) s.sampleButton(0, true, ++now);
    s.samplePot(0, now); for (uint8_t i = 0; i < 50; ++i) s.samplePot(4095, ++now);
    TEST_ASSERT_EQUAL_UINT8(0, t.n);
}

void test_source_detent_is_a_press_release_pair()
{
    Tape t; Source s; s.begin(Tape::emit, &t);
    s.configure(liveConfig());
    uint32_t now = 0;
    s.sampleEncoder(0, false, false, now);
    for (uint8_t i = 0; i < 4; ++i) s.sampleEncoder(0, CW_A[i], CW_B[i], ++now);
    TEST_ASSERT_EQUAL_UINT8(1, t.n);
    TEST_ASSERT_TRUE(t.items[0].e.action == Keys::Action::VolUp);
    TEST_ASSERT_TRUE(t.items[0].e.state == Keys::State::Press);
    TEST_ASSERT_TRUE(t.items[0].pair);                // VolUp needs its Release
    TEST_ASSERT_EQUAL_UINT8(Source::SRC_ID, t.items[0].e.srcId);
}

void test_source_wheel_actions_are_not_paired()
{
    // EncCw is released by the SINK (CanboxUartSink schedules it). A Release
    // from here as well would be the "wheel: press only" refusal at best.
    Tape t; Source s; s.begin(Tape::emit, &t);
    Config c = liveConfig();
    c.enc[0].cw = Binding{Keys::Action::EncCw, 0x81};
    s.configure(c);
    uint32_t now = 0;
    s.sampleEncoder(0, false, false, now);
    for (uint8_t i = 0; i < 4; ++i) s.sampleEncoder(0, CW_A[i], CW_B[i], ++now);
    TEST_ASSERT_EQUAL_UINT8(1, t.n);
    TEST_ASSERT_FALSE(t.items[0].pair);
    TEST_ASSERT_EQUAL_UINT8(0x81, t.items[0].e.codeHint);
}

static void hold(Source& s, bool down, uint32_t& now, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; ++i) s.sampleButton(0, down, ++now);
}

void test_button_without_hold_clicks_on_the_way_down()
{
    Tape t; Source s; s.begin(Tape::emit, &t);
    s.configure(liveConfig());                        // hold = None
    uint32_t now = 0;
    hold(s, false, now, 5);
    hold(s, true, now, 25);                           // past the 20 ms debounce
    TEST_ASSERT_EQUAL_UINT8(1, t.n);                  // already out, finger still down
    hold(s, true, now, 2000);
    hold(s, false, now, 50);
    TEST_ASSERT_EQUAL_UINT8(1, t.n);                  // and the release adds nothing
    TEST_ASSERT_TRUE(t.items[0].e.action == Keys::Action::Mute);
}

void test_button_with_hold_waits_and_picks_one()
{
    Tape t; Source s; s.begin(Tape::emit, &t);
    Config c = liveConfig();
    c.enc[0].hold = Binding{Keys::Action::Voice, 0}; c.enc[0].holdMs = 600;
    s.configure(c);
    uint32_t now = 0;
    hold(s, false, now, 5);

    hold(s, true, now, 100);                          // a short press...
    TEST_ASSERT_EQUAL_UINT8(0, t.n);                  // ...says nothing yet
    hold(s, false, now, 30);
    TEST_ASSERT_EQUAL_UINT8(1, t.n);
    TEST_ASSERT_TRUE(t.items[0].e.action == Keys::Action::Mute);

    hold(s, true, now, 700);                          // a long one
    TEST_ASSERT_EQUAL_UINT8(2, t.n);
    TEST_ASSERT_TRUE(t.items[1].e.action == Keys::Action::Voice);
    hold(s, false, now, 30);
    TEST_ASSERT_EQUAL_UINT8(2, t.n);                  // NOT a click as well
}

void test_button_held_through_boot_is_not_a_press()
{
    Tape t; Source s; s.begin(Tape::emit, &t);
    s.configure(liveConfig());
    uint32_t now = 0;
    hold(s, true, now, 3000);
    hold(s, false, now, 50);
    TEST_ASSERT_EQUAL_UINT8(0, t.n);
}

void test_reseed_after_an_outage_is_not_a_press_or_a_turn()
{
    // The expander drops out with the button down and comes back seconds later
    // (KnobService re-finds it and calls reseed()). The stale press must not
    // turn into a hold, nor its release into a click, and an encoder that moved
    // meanwhile is at a new position, not mid-turn.
    Tape t; Source s; s.begin(Tape::emit, &t);
    Config c = liveConfig();
    c.enc[0].hold = Binding{Keys::Action::Voice, 0}; c.enc[0].holdMs = 600;
    s.configure(c);
    uint32_t now = 0;
    hold(s, false, now, 5);
    s.sampleEncoder(0, false, false, now);
    hold(s, true, now, 100);                          // down, then the bus goes
    TEST_ASSERT_EQUAL_UINT8(0, t.n);

    now += 3000;                                      // no samples at all
    s.reseed();
    TEST_ASSERT_EQUAL_INT8(0, s.sampleEncoder(0, true, true, ++now));
    hold(s, true, now, 50);                           // still down: no hold
    hold(s, false, now, 30);                          // let go: no click
    TEST_ASSERT_EQUAL_UINT8(0, t.n);
    TEST_ASSERT_EQUAL_UINT32(0, s.encoder(0).errors());

    hold(s, true, now, 30);                           // the next press is a click again
    hold(s, false, now, 30);
    TEST_ASSERT_EQUAL_UINT8(1, t.n);
    TEST_ASSERT_TRUE(t.items[0].e.action == Keys::Action::Mute);
}

void test_button_bounce_is_one_click()
{
    Tape t; Source s; s.begin(Tape::emit, &t);
    s.configure(liveConfig());
    uint32_t now = 0;
    hold(s, false, now, 5);
    for (uint8_t i = 0; i < 8; ++i) { hold(s, true, now, 2); hold(s, false, now, 2); }
    hold(s, true, now, 40);
    hold(s, false, now, 40);
    TEST_ASSERT_EQUAL_UINT8(1, t.n);
}

void test_pot_steps_only_in_steps_mode_and_bursts_are_capped()
{
    Tape t; Source s; s.begin(Tape::emit, &t);
    Config c = liveConfig();
    c.potMode = PotMode::Brightness;
    s.configure(c);
    s.samplePot(0, 0);
    for (uint8_t i = 0; i < 60; ++i) s.samplePot(4095, i);
    TEST_ASSERT_EQUAL_UINT8(0, t.n);                  // a dimmer sends no keys
    TEST_ASSERT_EQUAL_UINT8(255, s.pot().level());    // ...but it does have a position

    c.potMode = PotMode::Steps;
    s.configure(c);                                   // re-seeds: no jump from the remap
    s.samplePot(4095, 100);
    TEST_ASSERT_EQUAL_UINT8(0, t.n);
    // Yanked end to end. Every sample may emit at most MAX_POT_BURST events.
    uint8_t before = t.n;
    s.samplePot(0, 101);
    TEST_ASSERT_TRUE(t.n - before <= 8);
    TEST_ASSERT_TRUE(t.n > 0 ? t.items[0].e.action == Keys::Action::VolDown : true);
}

// ---- lights ----------------------------------------------------------------------
static Config litConfig()
{
    Config c;
    c.ledEnabled = true; c.brightness = 255; c.reactHoldMs = 1000; c.levelSteps = 10;
    for (uint8_t z = 0; z < ZONES; ++z) {
        c.zone[z].count = 2; c.zone[z].react = React::Level;
        for (uint8_t i = 0; i < ZONE_LEDS; ++i) c.zone[z].led[i] = Rgb{255, 255, 255};
        c.zone[z].lo = Rgb{0, 0, 255}; c.zone[z].hi = Rgb{255, 0, 0};
    }
    return c;
}

static void run(Lights& l, uint32_t& now, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i += 20) { now += 20; l.tick(now); }
}

void test_lights_disabled_is_black()
{
    Lights l; Config c = litConfig(); c.ledEnabled = false;
    l.configure(c);
    uint32_t now = 0; run(l, now, 200);
    TEST_ASSERT_EQUAL_UINT8(4, l.count());
    for (uint8_t i = 0; i < 12; ++i) TEST_ASSERT_EQUAL_UINT8(0, l.pixels()[i]);
}

void test_lights_rest_at_base_and_tick_goes_quiet()
{
    Lights l; l.configure(litConfig());
    uint32_t now = 0;
    TEST_ASSERT_TRUE(l.tick(now += 20));              // first frame: black -> base
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[0]);
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[1]);
    // Nothing moving: no frames to send. This is what keeps the strip off the
    // wire for hours at a time.
    for (uint8_t i = 0; i < 50; ++i) TEST_ASSERT_FALSE(l.tick(now += 20));
}

void test_lights_level_follows_the_encoder_then_comes_home()
{
    Lights l; l.configure(litConfig());
    uint32_t now = 0; run(l, now, 100);

    // All the way up the virtual travel: 10 clicks long, starting in the middle.
    l.onTurn(ZONE_RIGHT, +10, now);
    run(l, now, 600);
    const uint8_t* right = l.pixels() + 2 * 3;        // left zone is first on the chain
    TEST_ASSERT_EQUAL_UINT8(255, right[0]);           // hi = red
    TEST_ASSERT_EQUAL_UINT8(0,   right[2]);
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[1]);      // the LEFT zone did not react

    run(l, now, 1500);                                // past reactHoldMs
    TEST_ASSERT_EQUAL_UINT8(255, right[0]);
    TEST_ASSERT_EQUAL_UINT8(255, right[1]);           // back to white
    TEST_ASSERT_EQUAL_UINT8(255, right[2]);
}

void test_lights_virtual_travel_is_clamped()
{
    Lights l; l.configure(litConfig());
    uint32_t now = 0;
    l.onTurn(ZONE_RIGHT, +100, now);
    TEST_ASSERT_EQUAL_UINT8(255, l.level(ZONE_RIGHT));
    l.onTurn(ZONE_RIGHT, -3, now);                    // and it leaves the end at once
    TEST_ASSERT_TRUE(l.level(ZONE_RIGHT) < 255);
    l.onTurn(ZONE_RIGHT, -100, now);
    TEST_ASSERT_EQUAL_UINT8(0, l.level(ZONE_RIGHT));
}

void test_lights_pot_first_level_is_not_a_movement()
{
    Lights l; l.configure(litConfig());
    uint32_t now = 0;
    l.setLevel(ZONE_LEFT, 255, now);                  // where the knob was at boot
    run(l, now, 300);
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[1]);      // still the base colour

    l.setLevel(ZONE_LEFT, 0, now);                    // now it MOVED, to the bottom
    run(l, now, 600);
    TEST_ASSERT_EQUAL_UINT8(0,   l.pixels()[0]);      // lo = blue
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[2]);
}

void test_lights_hold_zero_means_the_colour_is_the_position()
{
    Lights l; Config c = litConfig(); c.reactHoldMs = 0;
    l.configure(c);
    uint32_t now = 0;
    l.setLevel(ZONE_LEFT, 255, now);
    run(l, now, 60000);                               // a minute of nobody touching it
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[0]);      // hi = red, and it stays
    TEST_ASSERT_EQUAL_UINT8(0,   l.pixels()[1]);
}

void test_lights_right_first_swaps_the_chain()
{
    Lights l; Config c = litConfig(); c.rightFirst = true; c.reactHoldMs = 0;
    c.zone[ZONE_LEFT].count = 1; c.zone[ZONE_RIGHT].count = 3;
    l.configure(c);
    uint32_t now = 0;
    l.setLevel(ZONE_LEFT, 0, now);                    // left = blue
    l.onTurn(ZONE_RIGHT, +100, now);                  // right = red
    run(l, now, 600);
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[0]);      // pixel 0 is the RIGHT knob's
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[3 * 3 + 2]);   // pixel 3 is the left one's
}

void test_lights_master_dims_and_gamma_keeps_black_black()
{
    Lights l; l.configure(litConfig());
    uint32_t now = 0; run(l, now, 100);
    l.setMaster(128);
    run(l, now, 100);
    // Square law: half on the dial is a quarter of the light.
    TEST_ASSERT_UINT8_WITHIN(2, 64, l.pixels()[0]);
    l.setMaster(0);
    run(l, now, 100);
    TEST_ASSERT_EQUAL_UINT8(0, l.pixels()[0]);
}

void test_lights_flash_decays_to_base()
{
    Lights l; Config c = litConfig();
    c.zone[ZONE_RIGHT].react = React::Flash;
    for (uint8_t i = 0; i < ZONE_LEDS; ++i) c.zone[ZONE_RIGHT].led[i] = Rgb{100, 0, 0};
    l.configure(c);
    uint32_t now = 0; run(l, now, 100);
    const uint8_t* right = l.pixels() + 2 * 3;
    const uint8_t rest = right[0];

    l.onTurn(ZONE_RIGHT, +1, now);
    l.tick(now += 20);
    TEST_ASSERT_TRUE(right[0] > rest);                // kicked towards white
    TEST_ASSERT_TRUE(right[1] > 0);
    run(l, now, 2000);
    TEST_ASSERT_EQUAL_UINT8(rest, right[0]);          // and all the way back
    TEST_ASSERT_EQUAL_UINT8(0, right[1]);
}

void test_lights_every_led_has_its_own_colour()
{
    Lights l; Config c = litConfig();
    c.zone[ZONE_LEFT].react = React::None; c.zone[ZONE_LEFT].count = 3;
    c.zone[ZONE_LEFT].led[0] = Rgb{255, 0, 0};
    c.zone[ZONE_LEFT].led[1] = Rgb{0, 255, 0};
    c.zone[ZONE_LEFT].led[2] = Rgb{0, 0, 255};
    l.configure(c);
    uint32_t now = 0; run(l, now, 100);
    const uint8_t* p = l.pixels();
    TEST_ASSERT_EQUAL_UINT8(255, p[0]); TEST_ASSERT_EQUAL_UINT8(0, p[1]);
    TEST_ASSERT_EQUAL_UINT8(255, p[3 + 1]); TEST_ASSERT_EQUAL_UINT8(0, p[3]);
    TEST_ASSERT_EQUAL_UINT8(255, p[6 + 2]); TEST_ASSERT_EQUAL_UINT8(0, p[6]);
    TEST_ASSERT_EQUAL_UINT8(5, l.count());            // 3 left + 2 right
}

void test_lights_spin_walks_with_the_encoder_and_wraps()
{
    Lights l; Config c = litConfig();
    c.zone[ZONE_RIGHT].react = React::Spin; c.zone[ZONE_RIGHT].count = 3;
    c.zone[ZONE_RIGHT].hi = Rgb{0, 255, 0};           // the lit one is green
    l.configure(c);
    uint32_t now = 0; run(l, now, 100);
    TEST_ASSERT_EQUAL_UINT8(0, l.head(ZONE_RIGHT));

    l.onTurn(ZONE_RIGHT, +1, now);
    TEST_ASSERT_EQUAL_UINT8(1, l.head(ZONE_RIGHT));
    run(l, now, 400);
    const uint8_t* right = l.pixels() + 2 * 3;        // after the 2 left LEDs
    TEST_ASSERT_EQUAL_UINT8(0,   right[3 + 0]);       // LED 1: pure green
    TEST_ASSERT_EQUAL_UINT8(255, right[3 + 1]);
    TEST_ASSERT_TRUE(right[0] > 0 && right[0] < 60);  // LED 0: its own white, dimmed
    TEST_ASSERT_EQUAL_UINT8(right[0], right[6]);      // ...and LED 2 the same

    l.onTurn(ZONE_RIGHT, +2, now);                    // 1 -> 0, round the end
    TEST_ASSERT_EQUAL_UINT8(0, l.head(ZONE_RIGHT));
    l.onTurn(ZONE_RIGHT, -1, now);                    // and backwards off the start
    TEST_ASSERT_EQUAL_UINT8(2, l.head(ZONE_RIGHT));

    run(l, now, 2000);                                // left alone: back to white
    TEST_ASSERT_EQUAL_UINT8(255, right[0]);
    TEST_ASSERT_EQUAL_UINT8(255, right[3]);
}

void test_lights_spin_on_the_pot_is_a_position()
{
    Lights l; Config c = litConfig();
    c.zone[ZONE_LEFT].react = React::Spin; c.zone[ZONE_LEFT].count = 4;
    l.configure(c);
    uint32_t now = 0;
    l.setLevel(ZONE_LEFT, 0, now);
    TEST_ASSERT_EQUAL_UINT8(0, l.head(ZONE_LEFT));
    l.setLevel(ZONE_LEFT, 128, now);
    TEST_ASSERT_EQUAL_UINT8(2, l.head(ZONE_LEFT));
    l.setLevel(ZONE_LEFT, 255, now);                  // hard against the stop:
    TEST_ASSERT_EQUAL_UINT8(3, l.head(ZONE_LEFT));    // the LAST LED, not one past it
}

void test_lights_breathe_swells_and_never_goes_dark()
{
    Lights l; Config c = litConfig();
    c.animPeriod = 20;                                // 2 s
    c.zone[ZONE_LEFT].react = React::None; c.zone[ZONE_LEFT].anim = Anim::Breathe;
    l.configure(c);
    uint32_t now = 0;
    uint8_t lo = 255, hi = 0; uint32_t frames = 0;
    for (uint32_t i = 0; i < 4000; i += 20) {
        now += 20; if (l.tick(now)) ++frames;
        const uint8_t v = l.pixels()[0];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    TEST_ASSERT_TRUE(hi > 240);
    TEST_ASSERT_TRUE(lo > 10);                        // dim, not dark
    TEST_ASSERT_TRUE(hi - lo > 150);                  // and it really does move
    TEST_ASSERT_TRUE(frames > 100);                   // an animation is frames
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[2 * 3]);  // the other zone is still static
}

void test_lights_rainbow_spreads_the_wheel_across_the_zone()
{
    Lights l; Config c = litConfig();
    c.zone[ZONE_LEFT].react = React::None; c.zone[ZONE_LEFT].anim = Anim::Rainbow;
    c.zone[ZONE_LEFT].count = 3;
    l.configure(c);
    uint32_t now = 0; run(l, now, 400);
    const uint8_t* p = l.pixels();
    // Three LEDs a third of the wheel apart are never the same colour...
    TEST_ASSERT_TRUE(memcmp(p, p + 3, 3) != 0);
    TEST_ASSERT_TRUE(memcmp(p + 3, p + 6, 3) != 0);
    // ...and a saturated hue always has one channel at zero.
    for (uint8_t i = 0; i < 3; ++i) {
        const uint8_t* q = p + i * 3;
        const uint8_t mn = q[0] < q[1] ? (q[0] < q[2] ? q[0] : q[2]) : (q[1] < q[2] ? q[1] : q[2]);
        TEST_ASSERT_TRUE(mn < 8);
    }
}

void test_lights_reaction_sits_on_top_of_an_animation()
{
    // The two layers at once: a breathing zone that is turned must show the
    // gradient, steadily, and go back to breathing afterwards.
    Lights l; Config c = litConfig();
    c.zone[ZONE_RIGHT].anim = Anim::Breathe;
    l.configure(c);
    uint32_t now = 0; run(l, now, 100);
    l.onTurn(ZONE_RIGHT, +100, now);
    run(l, now, 500);
    const uint8_t* right = l.pixels() + 2 * 3;
    TEST_ASSERT_EQUAL_UINT8(255, right[0]);           // hi = red, not breathing
    TEST_ASSERT_EQUAL_UINT8(0,   right[1]);
    run(l, now, 3000);
    TEST_ASSERT_TRUE(right[1] > 0);                   // white again (and breathing)
}

// ---- footwells and the supply -----------------------------------------------------
static Config feetConfig(uint8_t n, uint16_t capMa)
{
    Config c = litConfig();                           // 2 + 2 knob LEDs, white, master 255
    c.zone[ZONE_LEFT].react = React::None; c.zone[ZONE_RIGHT].react = React::None;
    c.feetCount = n; c.feetColor = Rgb{255, 255, 255}; c.feetBrightness = 255;
    c.maxMilliamps = capMa;
    return c;
}

void test_feet_come_last_on_the_chain_and_fade_up_from_dark()
{
    Lights l; Config c = feetConfig(10, 20000);
    c.feetColor = Rgb{0, 255, 0};
    l.configure(c);
    TEST_ASSERT_EQUAL_UINT8(14, l.count());

    uint32_t now = 0;
    l.tick(now += 20);
    const uint8_t* feet = l.pixels() + 4 * 3;         // after the four knob LEDs
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[0]);      // the knobs are up at once...
    TEST_ASSERT_TRUE(feet[1] < 60);                   // ...the floor is still coming up

    run(l, now, 1000);
    for (uint8_t i = 0; i < 10; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0,   feet[i * 3]);
        TEST_ASSERT_EQUAL_UINT8(255, feet[i * 3 + 1]);
    }
    TEST_ASSERT_EQUAL_UINT8(0, l.pixels()[14 * 3 + 1]);   // and nothing past the end
}

void test_feet_have_their_own_dimmer_under_the_master()
{
    Lights l; Config c = feetConfig(4, 20000);
    c.feetBrightness = 128;
    l.configure(c);
    uint32_t now = 0; run(l, now, 1000);
    const uint8_t* feet = l.pixels() + 4 * 3;
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[0]);      // the knob rings are not dimmed by it
    TEST_ASSERT_UINT8_WITHIN(2, 64, feet[0]);         // square law: half is a quarter

    l.setMaster(128);                                 // the pot as a dimmer moves BOTH
    run(l, now, 1000);
    TEST_ASSERT_UINT8_WITHIN(2, 64, l.pixels()[0]);
    TEST_ASSERT_UINT8_WITHIN(2, 16, feet[0]);
}

void test_current_estimate_is_the_datasheet_arithmetic()
{
    const uint8_t white[3] = {255, 255, 255};
    const uint8_t black[3] = {0, 0, 0};
    TEST_ASSERT_EQUAL_UINT32(61, Lights::estimateMilliamps(white, 1));   // 3 x 20 + 1 idle
    TEST_ASSERT_EQUAL_UINT32(1,  Lights::estimateMilliamps(black, 1));   // black is not free
}

void test_limiter_holds_a_full_white_strip_under_the_cap()
{
    // The mistake this exists for: a hundred LEDs, white, everything at maximum,
    // on a supply that was sized for a night light.
    Lights l; l.configure(feetConfig(100, 2000));
    uint32_t now = 0; run(l, now, 1500);

    TEST_ASSERT_TRUE(l.milliampsWanted() > 6000);     // what it asked for
    TEST_ASSERT_TRUE(l.limited());
    TEST_ASSERT_TRUE(l.milliamps() <= 2000);          // what it got: never over
    TEST_ASSERT_TRUE(l.milliamps() > 1800);           // ...and not timidly under
    TEST_ASSERT_TRUE(Lights::estimateMilliamps(l.pixels(), l.count()) <= 2000);
}

void test_limiter_dims_without_shifting_the_hue()
{
    Lights l; Config c = feetConfig(100, 1000);
    c.feetColor = Rgb{255, 128, 0};
    l.configure(c);
    uint32_t now = 0; run(l, now, 1500);
    const uint8_t* p = l.pixels() + 4 * 3;
    TEST_ASSERT_TRUE(l.limited());
    TEST_ASSERT_EQUAL_UINT8(0, p[2]);
    // gamma(128) = 64, so the unlimited frame is 255 : 64. The ratio must survive.
    TEST_ASSERT_TRUE(p[0] > 0);
    const int ratio = (int)p[0] * 100 / (p[1] ? p[1] : 1);
    TEST_ASSERT_INT_WITHIN(25, 398, ratio);
    // Every LED the same: the limiter is one factor, not a per-pixel decision.
    TEST_ASSERT_EQUAL_UINT8_ARRAY(p, p + 99 * 3, 3);
}

void test_limiter_leaves_a_frame_under_the_cap_alone()
{
    Lights l; l.configure(feetConfig(10, 2000));      // 14 white LEDs: ~850 mA
    uint32_t now = 0; run(l, now, 1500);
    TEST_ASSERT_FALSE(l.limited());
    TEST_ASSERT_EQUAL_UINT16(l.milliampsWanted(), l.milliamps());
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[4 * 3]);
}

void test_limiter_covers_the_knob_rings_too()
{
    // One budget for one supply. A cap that only watched the footwells would be
    // a cap that sixteen white ring LEDs could walk straight past.
    Lights l; Config c = feetConfig(0, 200);
    c.zone[ZONE_LEFT].count = 8; c.zone[ZONE_RIGHT].count = 8;      // 16 x 61 = 976 mA
    l.configure(c);
    uint32_t now = 0; run(l, now, 500);
    TEST_ASSERT_TRUE(l.limited());
    TEST_ASSERT_TRUE(l.milliamps() <= 200);
    TEST_ASSERT_TRUE(l.pixels()[0] > 0);              // dimmed, not switched off
}

void test_feet_rainbow_runs_the_length_of_the_strip()
{
    Lights l; Config c = feetConfig(60, 20000);
    c.feetAnim = Anim::Rainbow;
    l.configure(c);
    uint32_t now = 0; run(l, now, 1000);
    const uint8_t* f = l.pixels() + 4 * 3;
    TEST_ASSERT_TRUE(memcmp(f, f + 20 * 3, 3) != 0);  // a third of the wheel apart
    TEST_ASSERT_TRUE(memcmp(f + 20 * 3, f + 40 * 3, 3) != 0);
}

void test_config_reads_a_v3_blob_with_one_encoder()
{
    // The bench board was saved under v3. Its right knob must come back exactly,
    // and the new left one must come up OFF rather than as garbage.
    Config a;
    a.enc[0].enabled = true; a.enc[0].ppd = 2; a.enc[0].cw = Binding{Keys::Action::VolUp, 0x14};
    a.ledEnabled = true; a.zone[1].count = 3;
    uint8_t v4[Config::BLOB_SIZE];
    a.toBlob(v4);
    uint8_t v3[Config::BLOB_SIZE_V3];
    memcpy(v3, v4, sizeof(v3));
    v3[0] = 3;
    Config b;
    TEST_ASSERT_TRUE(b.fromBlob(v3, sizeof(v3)));
    TEST_ASSERT_TRUE(b.enc[0].enabled);
    TEST_ASSERT_EQUAL_UINT8(2, b.enc[0].ppd);
    TEST_ASSERT_EQUAL_UINT8(0x14, b.enc[0].cw.code);
    TEST_ASSERT_EQUAL_UINT8(3, b.zone[1].count);
    TEST_ASSERT_FALSE(b.enc[1].enabled);
    TEST_ASSERT_TRUE(b.enc[1].cw.action == Keys::Action::VolUp);   // the default binding
}

void test_the_two_encoders_are_independent()
{
    Tape t; Source s; s.begin(Tape::emit, &t);
    Config c = liveConfig();
    c.enc[1].enabled = true;
    c.enc[1].cw = Binding{Keys::Action::SrcNext, 0}; c.enc[1].click = Binding{Keys::Action::Radio, 0};
    s.configure(c);
    uint32_t now = 0;
    s.sampleEncoder(0, false, false, now); s.sampleEncoder(1, false, false, now);
    for (uint8_t i = 0; i < 4; ++i) s.sampleEncoder(1, CW_A[i], CW_B[i], ++now);   // LEFT clicks
    TEST_ASSERT_EQUAL_UINT8(1, t.n);
    TEST_ASSERT_TRUE(t.items[0].e.action == Keys::Action::SrcNext);
    TEST_ASSERT_EQUAL_UINT32(1, s.encoder(1).detents());
    TEST_ASSERT_EQUAL_UINT32(0, s.encoder(0).detents());              // the right did not move
    for (uint8_t i = 0; i < 5;  ++i) { s.sampleButton(1, false, ++now); s.sampleButton(0, false, now); }  // seed: released
    for (uint8_t i = 0; i < 30; ++i) { s.sampleButton(1, true, ++now);  s.sampleButton(0, false, now); }
    TEST_ASSERT_EQUAL_UINT8(2, t.n);
    TEST_ASSERT_TRUE(t.items[1].e.action == Keys::Action::Radio);
    TEST_ASSERT_EQUAL_UINT32(1, s.clicks(1));
    TEST_ASSERT_EQUAL_UINT32(0, s.clicks(0));
    // An index past the end is refused, not indexed.
    TEST_ASSERT_EQUAL_INT8(0, s.sampleEncoder(7, true, false, ++now));
}

void test_lights_sweep_runs_one_dot_round_the_ring_per_click()
{
    Lights l; Config c = litConfig();
    c.zone[ZONE_RIGHT].react = React::Sweep; c.zone[ZONE_RIGHT].count = 5; c.maxMilliamps = 20000;   // 7 white LEDs > the USB cap
    c.zone[ZONE_RIGHT].hi = Rgb{0, 255, 0};
    l.configure(c);
    uint32_t now = 0; run(l, now, 100);
    const uint8_t* right = l.pixels() + 2 * 3;        // after the 2 left LEDs
    l.onTurn(ZONE_RIGHT, +1, now);
    l.tick(now + 5);                                  // t=5 ms: LED 0
    TEST_ASSERT_TRUE(right[0 * 3 + 1] > right[4 * 3 + 1]);   // 0 greener than 4
    for (uint8_t k = 0; k < 8; ++k) l.tick(now + 20 + k * 10);   // ease along
    l.tick(now + 135);                                // t=135 ms: LED 4
    TEST_ASSERT_TRUE(right[4 * 3 + 1] > right[0 * 3 + 1]);
    now += 200; run(l, now, 1000);                    // over: back to rest (white)
    TEST_ASSERT_EQUAL_UINT8(255, right[0]);
    TEST_ASSERT_EQUAL_UINT8(255, right[4 * 3]);
    // Anti-clockwise runs the other way: LED 4 first.
    l.onTurn(ZONE_RIGHT, -1, now);
    for (uint8_t k = 0; k < 4; ++k) l.tick(now + 2 + k);
    TEST_ASSERT_TRUE(right[4 * 3 + 1] > right[0 * 3 + 1]);
}

void test_lights_trail_fades_behind_the_head()
{
    Lights l; Config c = litConfig();
    c.zone[ZONE_RIGHT].react = React::Trail; c.zone[ZONE_RIGHT].count = 5; c.reactHoldMs = 0; c.maxMilliamps = 20000;
    c.zone[ZONE_RIGHT].hi = Rgb{0, 0, 255};           // blue head over a white rest
    l.configure(c);
    uint32_t now = 0; run(l, now, 100);
    l.onTurn(ZONE_RIGHT, +1, now); l.onTurn(ZONE_RIGHT, +1, now); l.onTurn(ZONE_RIGHT, +1, now);
    run(l, now, 600);
    const uint8_t* right = l.pixels() + 2 * 3;
    TEST_ASSERT_EQUAL_UINT8(3, l.head(ZONE_RIGHT));
    // Head: pure hi. Behind it (2, then 1): whiter and whiter. Far side (4): dim rest.
    TEST_ASSERT_EQUAL_UINT8(0, right[3 * 3]);         // head has no red
    TEST_ASSERT_TRUE(right[2 * 3] > right[3 * 3]);    // one behind: some red back
    TEST_ASSERT_TRUE(right[1 * 3] > right[2 * 3]);    // two behind: more
    TEST_ASSERT_TRUE(right[4 * 3] < right[1 * 3]);    // ahead of the head: dim rest
}

void test_lights_both_zones_react_to_their_own_encoder()
{
    Lights l; l.configure(litConfig());
    uint32_t now = 0; run(l, now, 100);
    l.onTurn(ZONE_LEFT, +100, now);                   // the LEFT knob, all the way up
    run(l, now, 600);
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[0]);      // left zone: hi = red
    TEST_ASSERT_EQUAL_UINT8(0,   l.pixels()[2]);
    TEST_ASSERT_EQUAL_UINT8(255, l.pixels()[2 * 3 + 2]);   // right zone untouched: white
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_quad_one_click_is_one_step);
    RUN_TEST(test_quad_first_sample_is_a_position_not_a_step);
    RUN_TEST(test_quad_bounce_cancels);
    RUN_TEST(test_quad_inverted_swaps_direction);
    RUN_TEST(test_quad_two_pulses_per_detent);
    RUN_TEST(test_quad_double_jump_is_counted_not_guessed);
    RUN_TEST(test_quad_missed_sample_mid_turn_keeps_the_click);
    RUN_TEST(test_quad_rest_clears_a_missed_quarter);
    RUN_TEST(test_pot_first_reading_says_nothing);
    RUN_TEST(test_pot_full_sweep_is_zones_minus_one);
    RUN_TEST(test_pot_noise_on_a_boundary_does_not_chatter);
    RUN_TEST(test_pot_inverted_flips_direction_not_calibration);
    RUN_TEST(test_pot_bad_calibration_falls_back_to_full_range);
    RUN_TEST(test_config_defaults_are_silent);
    RUN_TEST(test_config_blob_round_trip);
    RUN_TEST(test_config_refuses_a_foreign_blob);
    RUN_TEST(test_config_sanitize_strips_phases_and_caps_leds);
    RUN_TEST(test_config_reads_a_v3_blob_with_one_encoder);
    RUN_TEST(test_the_two_encoders_are_independent);
    RUN_TEST(test_lights_both_zones_react_to_their_own_encoder);
    RUN_TEST(test_lights_sweep_runs_one_dot_round_the_ring_per_click);
    RUN_TEST(test_lights_trail_fades_behind_the_head);
    RUN_TEST(test_source_disabled_emits_nothing);
    RUN_TEST(test_source_detent_is_a_press_release_pair);
    RUN_TEST(test_source_wheel_actions_are_not_paired);
    RUN_TEST(test_button_without_hold_clicks_on_the_way_down);
    RUN_TEST(test_button_with_hold_waits_and_picks_one);
    RUN_TEST(test_button_held_through_boot_is_not_a_press);
    RUN_TEST(test_reseed_after_an_outage_is_not_a_press_or_a_turn);
    RUN_TEST(test_button_bounce_is_one_click);
    RUN_TEST(test_pot_steps_only_in_steps_mode_and_bursts_are_capped);
    RUN_TEST(test_lights_disabled_is_black);
    RUN_TEST(test_lights_rest_at_base_and_tick_goes_quiet);
    RUN_TEST(test_lights_level_follows_the_encoder_then_comes_home);
    RUN_TEST(test_lights_virtual_travel_is_clamped);
    RUN_TEST(test_lights_pot_first_level_is_not_a_movement);
    RUN_TEST(test_lights_hold_zero_means_the_colour_is_the_position);
    RUN_TEST(test_lights_right_first_swaps_the_chain);
    RUN_TEST(test_lights_master_dims_and_gamma_keeps_black_black);
    RUN_TEST(test_lights_flash_decays_to_base);
    RUN_TEST(test_lights_every_led_has_its_own_colour);
    RUN_TEST(test_lights_spin_walks_with_the_encoder_and_wraps);
    RUN_TEST(test_lights_spin_on_the_pot_is_a_position);
    RUN_TEST(test_lights_breathe_swells_and_never_goes_dark);
    RUN_TEST(test_lights_rainbow_spreads_the_wheel_across_the_zone);
    RUN_TEST(test_lights_reaction_sits_on_top_of_an_animation);
    RUN_TEST(test_config_supply_default_is_what_a_usb_port_can_give);
    RUN_TEST(test_feet_come_last_on_the_chain_and_fade_up_from_dark);
    RUN_TEST(test_feet_have_their_own_dimmer_under_the_master);
    RUN_TEST(test_current_estimate_is_the_datasheet_arithmetic);
    RUN_TEST(test_limiter_holds_a_full_white_strip_under_the_cap);
    RUN_TEST(test_limiter_dims_without_shifting_the_hue);
    RUN_TEST(test_limiter_leaves_a_frame_under_the_cap_alone);
    RUN_TEST(test_limiter_covers_the_knob_rings_too);
    RUN_TEST(test_feet_rainbow_runs_the_length_of_the_strip);
    return UNITY_END();
}
