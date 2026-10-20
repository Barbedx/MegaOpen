// Key routing: which class an action belongs to, and which destinations act on
// it. Pure table logic, and worth host tests precisely because the failure mode
// on hardware is silence — a press that goes nowhere looks exactly like a press
// that was not detected, a head unit that ignored it, or a broken wire.
#include <unity.h>
#include <string.h>
#include "keys/KeyAction.h"
#include "keys/KeyDests.h"
#include "sinks/CanboxUartSink.h"
#include "proto/canbox/Rav4Profile.h"

using namespace Keys;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// classOf
// ---------------------------------------------------------------------------

void test_every_class_is_the_device_that_owns_the_action()
{
    TEST_ASSERT_EQUAL_UINT8(CLASS_VOLUME,    classOf(Action::VolUp));
    TEST_ASSERT_EQUAL_UINT8(CLASS_VOLUME,    classOf(Action::VolDown));
    TEST_ASSERT_EQUAL_UINT8(CLASS_VOLUME,    classOf(Action::Mute));

    TEST_ASSERT_EQUAL_UINT8(CLASS_TRANSPORT, classOf(Action::Ok));
    TEST_ASSERT_EQUAL_UINT8(CLASS_TRANSPORT, classOf(Action::Load));
    TEST_ASSERT_EQUAL_UINT8(CLASS_TRANSPORT, classOf(Action::SrcNext));
    TEST_ASSERT_EQUAL_UINT8(CLASS_TRANSPORT, classOf(Action::SrcPrev));
    TEST_ASSERT_EQUAL_UINT8(CLASS_TRANSPORT, classOf(Action::EncCw));
    TEST_ASSERT_EQUAL_UINT8(CLASS_TRANSPORT, classOf(Action::EncCcw));

    TEST_ASSERT_EQUAL_UINT8(CLASS_PHONE,     classOf(Action::Phone));
    TEST_ASSERT_EQUAL_UINT8(CLASS_PHONE,     classOf(Action::Voice));

    TEST_ASSERT_EQUAL_UINT8(CLASS_UI,        classOf(Action::Mode));
    TEST_ASSERT_EQUAL_UINT8(CLASS_UI,        classOf(Action::Radio));
}

// The three encoder phases are matrix POSITIONS that EncoderDecoder consumes.
// They are never dispatched — and belonging to no class means that if one ever
// leaked through, no destination would act on it.
void test_encoder_phases_belong_to_no_class()
{
    TEST_ASSERT_EQUAL_UINT8(CLASS_NONE, classOf(Action::EncPhaseA));
    TEST_ASSERT_EQUAL_UINT8(CLASS_NONE, classOf(Action::EncPhaseB));
    TEST_ASSERT_EQUAL_UINT8(CLASS_NONE, classOf(Action::EncPhaseC));
    TEST_ASSERT_EQUAL_UINT8(CLASS_NONE, classOf(Action::None));
}

// Every dispatchable action must land in exactly one class. A new action added
// to the enum without a case in classOf() falls to the default and silently
// stops being routable anywhere — this is the test that notices.
void test_every_real_action_has_exactly_one_class()
{
    for (uint8_t i = 1; i < (uint8_t)Action::_Count; ++i) {
        const Action a = (Action)i;
        if (isEncoderPhase(a)) continue;
        const uint8_t c = classOf(a);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(CLASS_NONE, c, name(a));
        // Exactly one bit set.
        TEST_ASSERT_EQUAL_MESSAGE(0, c & (c - 1), name(a));
    }
}

// ---------------------------------------------------------------------------
// The UART sink honouring its mask
// ---------------------------------------------------------------------------

struct Cap {
    static constexpr int MAX = 32;
    uint8_t code[MAX];
    uint8_t state[MAX];
    int n = 0;
    void reset() { n = 0; }
};

static Cap g_cap;

static void capture(const uint8_t* frame, uint8_t len, void*)
{
    // Raise frame: 0x2E 0xNN <cmd> <payload...> — the key payload is code,state.
    if (len < 6 || g_cap.n >= Cap::MAX) return;
    g_cap.code[g_cap.n]  = frame[3];
    g_cap.state[g_cap.n] = frame[4];
    ++g_cap.n;
}

static Sinks::CanboxUartSink makeSink()
{
    Sinks::CanboxUartSink s;
    s.begin(capture, nullptr);
    s.setEnabled(true);
    g_cap.reset();
    return s;
}

static Event press(Action a, uint32_t t = 0)
{
    Event e; e.action = a; e.state = State::Press; e.tMs = t; return e;
}

void test_the_uart_sink_emits_what_its_mask_allows()
{
    Sinks::CanboxUartSink s = makeSink();
    s.setClasses(CLASS_VOLUME);

    s.onKey(press(Action::VolUp));
    TEST_ASSERT_EQUAL_INT(1, g_cap.n);

    // Transport is not in the mask, so nothing goes on the wire.
    g_cap.reset();
    s.onKey(press(Action::SrcNext));
    TEST_ASSERT_EQUAL_INT(0, g_cap.n);
}

void test_an_empty_mask_is_a_destination_switched_off()
{
    Sinks::CanboxUartSink s = makeSink();
    s.setClasses(CLASS_NONE);
    s.onKey(press(Action::VolUp));
    s.onKey(press(Action::SrcNext));
    s.onKey(press(Action::Mode));
    TEST_ASSERT_EQUAL_INT(0, g_cap.n);
}

void test_class_all_lets_everything_through()
{
    Sinks::CanboxUartSink s = makeSink();
    s.setClasses(CLASS_ALL);
    s.onKey(press(Action::VolUp));
    s.onKey(press(Action::SrcNext));
    TEST_ASSERT_EQUAL_INT(2, g_cap.n);
}

// The masks are independent: two destinations may both claim a class, which is
// what "a press can go to more than one place" means. Nothing here should
// prevent it — the overlap is a decision for the person reading the matrix, not
// something the router should quietly resolve.
void test_two_destinations_may_claim_the_same_class()
{
    Sinks::CanboxUartSink a = makeSink();
    a.setClasses(CLASS_TRANSPORT);
    a.onKey(press(Action::SrcNext));
    const int first = g_cap.n;

    Sinks::CanboxUartSink b = makeSink();
    b.setClasses(CLASS_TRANSPORT | CLASS_VOLUME);
    b.onKey(press(Action::SrcNext));

    TEST_ASSERT_EQUAL_INT(1, first);
    TEST_ASSERT_EQUAL_INT(1, g_cap.n);
}

// Routing is checked BEFORE the encoder's scheduled release is armed. A class
// switched off between a press and its release would otherwise leave the head
// unit holding a key down that nothing will ever lift.
void test_a_filtered_encoder_step_schedules_no_release()
{
    Sinks::CanboxUartSink s = makeSink();
    s.setClasses(CLASS_VOLUME);          // transport filtered out

    s.onKey(press(Action::EncCw, 1000));
    TEST_ASSERT_EQUAL_INT(0, g_cap.n);

    // Run the clock well past any pulse that might have been scheduled.
    s.tick(2000);
    s.tick(5000);
    TEST_ASSERT_EQUAL_INT(0, g_cap.n);
}

// The link switch and the routing mask are different questions, and turning the
// link off must silence a destination whose mask still allows the class.
void test_the_link_switch_overrides_the_mask()
{
    Sinks::CanboxUartSink s = makeSink();
    s.setClasses(CLASS_ALL);
    s.setEnabled(false);
    s.onKey(press(Action::VolUp));
    TEST_ASSERT_EQUAL_INT(0, g_cap.n);
}

// ---------------------------------------------------------------------------
// keyDests: where the wiring check says a key went
// ---------------------------------------------------------------------------

// A destination takes the key only when it is live AND its mask has the class.
void test_dests_need_both_the_mask_and_a_live_sink()
{
    DestState d[DEST_COUNT];
    d[DEST_UART] = {CLASS_ALL, true, false};
    d[DEST_AMS]  = {CLASS_TRANSPORT, false, true};   // masked in, not in the chain
    d[DEST_HID]  = {CLASS_VOLUME, true, true};       // live, other class
    d[DEST_USB]  = {CLASS_TRANSPORT, true, true};
    TEST_ASSERT_EQUAL_HEX8((1u << DEST_UART) | (1u << DEST_USB),
                           keyDests(CLASS_TRANSPORT, false, d));
    TEST_ASSERT_EQUAL_HEX8((1u << DEST_UART) | (1u << DEST_HID),
                           keyDests(CLASS_VOLUME, false, d));
}

// An asLong row's first edge is a Long, which the BLE and USB sinks ignore.
void test_a_long_reaches_only_the_sinks_that_take_one()
{
    DestState d[DEST_COUNT];
    d[DEST_UART] = {CLASS_ALL, true, false};
    d[DEST_AMS]  = {CLASS_ALL, true, true};
    d[DEST_RES]  = {CLASS_ALL, true, false};
    TEST_ASSERT_EQUAL_HEX8((1u << DEST_UART) | (1u << DEST_RES), keyDests(CLASS_VOLUME, true, d));
    TEST_ASSERT_EQUAL_HEX8((1u << DEST_UART) | (1u << DEST_AMS) | (1u << DEST_RES),
                           keyDests(CLASS_VOLUME, false, d));
}

// The USB keyboard has no usages for phone or ui keys: masked in, still not routed.
void test_usb_takes_only_transport_and_volume()
{
    DestState d[DEST_COUNT];
    d[DEST_USB] = {CLASS_ALL, true, true};
    TEST_ASSERT_EQUAL_HEX8(1u << DEST_USB, keyDests(CLASS_TRANSPORT, false, d));
    TEST_ASSERT_EQUAL_HEX8(1u << DEST_USB, keyDests(CLASS_VOLUME, false, d));
    TEST_ASSERT_EQUAL_HEX8(0, keyDests(CLASS_PHONE, false, d));
    TEST_ASSERT_EQUAL_HEX8(0, keyDests(CLASS_UI, false, d));
}

// An asLong row: BLE (AMS, HID) and USB refuse the Long, the UART takes it.
void test_as_long_is_refused_by_ble_and_usb()
{
    DestState d[DEST_COUNT];
    d[DEST_UART] = {CLASS_ALL, true, false};
    d[DEST_AMS]  = {CLASS_ALL, true, true};
    d[DEST_HID]  = {CLASS_ALL, true, true};
    d[DEST_USB]  = {CLASS_ALL, true, true};
    TEST_ASSERT_EQUAL_HEX8(1u << DEST_UART, keyDests(CLASS_TRANSPORT, true, d));
}

// No class (None, an encoder phase) goes nowhere, whatever the masks say.
void test_no_class_goes_nowhere()
{
    DestState d[DEST_COUNT];
    for (auto& x : d) x = {0xFF, true, false};
    TEST_ASSERT_EQUAL_HEX8(0, keyDests(classOf(Action::None), false, d));
}

// The ids are the catalog's, in the bit order the report uses.
void test_dest_ids_are_the_catalogs()
{
    TEST_ASSERT_EQUAL_STRING("uart", DEST_IDS[DEST_UART]);
    TEST_ASSERT_EQUAL_STRING("ams",  DEST_IDS[DEST_AMS]);
    TEST_ASSERT_EQUAL_STRING("hid",  DEST_IDS[DEST_HID]);
    TEST_ASSERT_EQUAL_STRING("res",  DEST_IDS[DEST_RES]);
    TEST_ASSERT_EQUAL_STRING("usb",  DEST_IDS[DEST_USB]);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_class_is_the_device_that_owns_the_action);
    RUN_TEST(test_encoder_phases_belong_to_no_class);
    RUN_TEST(test_every_real_action_has_exactly_one_class);
    RUN_TEST(test_the_uart_sink_emits_what_its_mask_allows);
    RUN_TEST(test_an_empty_mask_is_a_destination_switched_off);
    RUN_TEST(test_class_all_lets_everything_through);
    RUN_TEST(test_two_destinations_may_claim_the_same_class);
    RUN_TEST(test_a_filtered_encoder_step_schedules_no_release);
    RUN_TEST(test_the_link_switch_overrides_the_mask);
    RUN_TEST(test_dests_need_both_the_mask_and_a_live_sink);
    RUN_TEST(test_a_long_reaches_only_the_sinks_that_take_one);
    RUN_TEST(test_no_class_goes_nowhere);
    RUN_TEST(test_usb_takes_only_transport_and_volume);
    RUN_TEST(test_as_long_is_refused_by_ble_and_usb);
    RUN_TEST(test_dest_ids_are_the_catalogs);
    return UNITY_END();
}
