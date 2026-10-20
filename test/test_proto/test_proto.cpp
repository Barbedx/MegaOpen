#include <unity.h>
#include <string.h>
#include "proto/canbox/RaiseFramer.h"
#include "proto/canbox/Rav4Profile.h"
#include "proto/canbox/Rav4Catalog.h"
#include "proto/canbox/Rav4Emitter.h"
#include "keys/KeyMap.h"
#include "sinks/CanboxUartSink.h"

void setUp() {}
void tearDown() {}

// ---- framing ----------------------------------------------------------------
void test_frame_golden_vector()
{
    // VOL+ pressed: 2E 20 02 01 01 DB
    const uint8_t payload[2] = {Canbox::RAV4_VOL_UP, Canbox::KEY_PRESS};
    uint8_t out[Canbox::MAX_FRAME];
    uint8_t n = Canbox::buildFrame(Canbox::CMD_KEY, payload, 2, out);

    const uint8_t expect[6] = {0x2E, 0x20, 0x02, 0x01, 0x01, 0xDB};
    TEST_ASSERT_EQUAL_UINT8(6, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, out, 6);
}

void test_checksum_is_sum_xor_ff()
{
    const uint8_t p[3] = {0x11, 0x22, 0x33};
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((0x7D + 0x03 + 0x11 + 0x22 + 0x33) ^ 0xFF),
                            Canbox::checksum(0x7D, 3, p));
}

void test_oversized_payload_is_refused()
{
    uint8_t big[64] = {0};
    uint8_t out[Canbox::MAX_FRAME];
    TEST_ASSERT_EQUAL_UINT8(0, Canbox::buildFrame(0x20, big, 64, out));
}

// ---- inbound parser ---------------------------------------------------------
static uint8_t g_cmd; static uint8_t g_len; static uint8_t g_pl[8]; static int g_frames;
static int g_junk;

static void frameCb(uint8_t cmd, const uint8_t* p, uint8_t len, void*)
{
    g_cmd = cmd; g_len = len; g_frames++;
    memcpy(g_pl, p, len < sizeof(g_pl) ? len : sizeof(g_pl));
}
static void junkCb(uint8_t, void*) { g_junk++; }

void test_rx_parses_frame_after_junk()
{
    g_frames = g_junk = 0;
    Canbox::RxParser rx;
    rx.onFrame(frameCb, nullptr);
    rx.onJunk(junkCb, nullptr);

    const uint8_t stream[] = {0xAA, 0xBB, 0x2E, 0x20, 0x02, 0x01, 0x01, 0xDB};
    for (uint8_t b : stream) rx.feed(b);

    TEST_ASSERT_EQUAL_INT(2, g_junk);
    TEST_ASSERT_EQUAL_INT(1, g_frames);
    TEST_ASSERT_EQUAL_UINT8(0x20, g_cmd);
    TEST_ASSERT_EQUAL_UINT8(2, g_len);
    TEST_ASSERT_EQUAL_UINT8(0x01, g_pl[0]);
}

// A long undocumented frame (a track title, say) must not desynchronise the
// parser — least of all by resyncing on a 0x2E that happens to be inside the
// text, which would manufacture a stream of plausible garbage frames.
void test_rx_survives_a_frame_longer_than_the_buffer()
{
    g_frames = g_junk = 0;
    Canbox::RxParser rx;
    rx.onFrame(frameCb, nullptr);
    rx.onJunk(junkCb, nullptr);

    const uint8_t LEN = 200;                       // > MAX_RX_PAYLOAD
    uint8_t sum = (uint8_t)(0xC7 + LEN);
    rx.feed(0x2E); rx.feed(0xC7); rx.feed(LEN);
    for (uint8_t i = 0; i < LEN; ++i) {
        const uint8_t byte = (i == 40) ? 0x2E : (uint8_t)('A' + (i % 26));
        sum = (uint8_t)(sum + byte);
        rx.feed(byte);
    }
    rx.feed((uint8_t)(sum ^ 0xFF));

    TEST_ASSERT_EQUAL_INT(1, g_frames);            // checksum still verified
    TEST_ASSERT_EQUAL_UINT8(0xC7, g_cmd);
    TEST_ASSERT_EQUAL_UINT16(LEN, rx.lastTotalLen());
    TEST_ASSERT_EQUAL_UINT32(1, rx.truncated());
    TEST_ASSERT_EQUAL_INT(0, g_junk);              // the embedded 0x2E stayed payload

    // ...and the stream is still aligned afterwards.
    const uint8_t next[] = {0x2E, 0x20, 0x02, 0x01, 0x01, 0xDB};
    for (uint8_t b : next) rx.feed(b);
    TEST_ASSERT_EQUAL_INT(2, g_frames);
    TEST_ASSERT_EQUAL_UINT8(0x20, g_cmd);
}

void test_rx_keeps_a_long_text_payload_readable()
{
    g_frames = 0;
    Canbox::RxParser rx;
    rx.onFrame(frameCb, nullptr);

    const char* text = "Rammstein - Sonne";
    const uint8_t len = (uint8_t)strlen(text);
    uint8_t sum = (uint8_t)(0xC5 + len);
    rx.feed(0x2E); rx.feed(0xC5); rx.feed(len);
    for (uint8_t i = 0; i < len; ++i) { sum = (uint8_t)(sum + text[i]); rx.feed((uint8_t)text[i]); }
    rx.feed((uint8_t)(sum ^ 0xFF));

    TEST_ASSERT_EQUAL_INT(1, g_frames);
    TEST_ASSERT_EQUAL_UINT8(len, g_len);
    TEST_ASSERT_EQUAL_UINT8('R', g_pl[0]);
}

void test_rx_rejects_bad_checksum()
{
    g_frames = 0;
    Canbox::RxParser rx;
    rx.onFrame(frameCb, nullptr);
    const uint8_t bad[] = {0x2E, 0x20, 0x02, 0x01, 0x01, 0x00};
    for (uint8_t b : bad) rx.feed(b);
    TEST_ASSERT_EQUAL_INT(0, g_frames);
    TEST_ASSERT_EQUAL_UINT32(1, rx.badChecksums());
}

// ---- key map ----------------------------------------------------------------
void test_defaults_match_the_megane_keypad()
{
    Keys::KeyMap m;
    m.loadDefaults();
    TEST_ASSERT_TRUE(m.slot(0).action == Keys::Action::VolDown);
    TEST_ASSERT_TRUE(m.slot(2).action == Keys::Action::VolUp);
    TEST_ASSERT_TRUE(m.slot(5).action == Keys::Action::Mute);

    uint8_t a, b, c;
    m.phases(a, b, c);
    TEST_ASSERT_EQUAL_UINT8(6, a);
    TEST_ASSERT_EQUAL_UINT8(7, b);
    TEST_ASSERT_EQUAL_UINT8(8, c);
    TEST_ASSERT_EQUAL_UINT16(0b111000000, m.fastMask());
}

void test_keymap_blob_roundtrip()
{
    Keys::KeyMap a;
    a.loadDefaults();
    a.setSlot(4, Keys::Slot{Keys::Action::Phone, 0x88});
    a.setEncCode(true, 0x85);

    uint8_t blob[Keys::KeyMap::BLOB_SIZE];
    a.toBlob(blob);

    Keys::KeyMap b;
    TEST_ASSERT_TRUE(b.fromBlob(blob, sizeof(blob)));
    TEST_ASSERT_TRUE(b.slot(4).action == Keys::Action::Phone);
    TEST_ASSERT_EQUAL_UINT8(0x88, b.slot(4).code);
    TEST_ASSERT_EQUAL_UINT8(0x85, b.encCode(true));
}

void test_blob_with_unknown_action_is_rejected()
{
    uint8_t blob[Keys::KeyMap::BLOB_SIZE] = {0};
    blob[0] = 0xFE;                       // action id from another firmware
    Keys::KeyMap m;
    TEST_ASSERT_FALSE(m.fromBlob(blob, sizeof(blob)));
}

// ---- sink -------------------------------------------------------------------
static uint8_t g_tx[64]; static uint8_t g_txLen; static int g_writes;
static void capture(const uint8_t* f, uint8_t n, void*)
{
    memcpy(g_tx, f, n); g_txLen = n; g_writes++;
}

// A wheel step mapped onto an ORDINARY key code (someone overrides the rotary
// default with, say, ENTER) still has no release of its own, so the sink has to
// schedule one. The rotary path is covered separately below.
void test_encoder_on_a_plain_code_is_a_scheduled_pulse()
{
    g_writes = 0;
    Sinks::CanboxUartSink s;
    s.begin(capture, nullptr);
    s.setPulseMs(60);

    Keys::Event e;
    e.action   = Keys::Action::EncCw;
    e.state    = Keys::State::Press;
    e.tMs      = 1000;
    e.codeHint = Canbox::RAV4_ENTER;       // not a knob code
    s.onKey(e);

    TEST_ASSERT_EQUAL_INT(1, g_writes);
    TEST_ASSERT_EQUAL_UINT8(Canbox::RAV4_ENTER, g_tx[3]);
    TEST_ASSERT_EQUAL_UINT8(Canbox::KEY_PRESS, g_tx[4]);

    s.tick(1030);                          // too early
    TEST_ASSERT_EQUAL_INT(1, g_writes);

    s.tick(1061);                          // release lands on its own
    TEST_ASSERT_EQUAL_INT(2, g_writes);
    TEST_ASSERT_EQUAL_UINT8(Canbox::KEY_RELEASE, g_tx[4]);
}

void test_code_override_beats_the_profile_default()
{
    g_writes = 0;
    Sinks::CanboxUartSink s;
    s.begin(capture, nullptr);

    Keys::Event e;
    e.action   = Keys::Action::Mode;
    e.state    = Keys::State::Press;
    e.codeHint = 0x42;
    s.onKey(e);

    TEST_ASSERT_EQUAL_UINT8(0x42, g_tx[3]);
}

void test_wheel_sends_a_detent_count_and_no_release()
{
    g_writes = 0;
    Sinks::CanboxUartSink s;
    s.begin(capture, nullptr);

    Keys::Event e;
    e.action = Keys::Action::EncCw;
    e.state  = Keys::State::Press;
    e.tMs    = 1000;
    s.onKey(e);

    // 0x85 is a rotary code: the second byte is a detent count, and the head unit
    // returns early after looping over it. A release frame would be a no-op.
    TEST_ASSERT_EQUAL_INT(1, g_writes);
    TEST_ASSERT_EQUAL_UINT8(Canbox::RAV4_KNOB_TUNE_UP, g_tx[3]);
    TEST_ASSERT_EQUAL_UINT8(1, g_tx[4]);
    s.tick(2000);
    TEST_ASSERT_EQUAL_INT(1, g_writes);
}

void test_mute_is_a_long_press_of_source()
{
    g_writes = 0;
    Sinks::CanboxUartSink s;
    s.begin(capture, nullptr);

    Keys::Event e;
    e.action = Keys::Action::Mute;
    e.state  = Keys::State::Press;
    s.onKey(e);
    TEST_ASSERT_EQUAL_UINT8(Canbox::RAV4_SOURCE, g_tx[3]);
    TEST_ASSERT_EQUAL_UINT8(Canbox::KEY_LONG, g_tx[4]);
}

void test_rx_acks_every_frame_including_a_corrupt_one()
{
    static int acks = 0;
    acks = 0;
    Canbox::RxParser rx;
    rx.onAck([](void*) { acks++; }, nullptr);

    const uint8_t good[] = {0x2E, 0x20, 0x02, 0x01, 0x01, 0xDB};
    for (uint8_t b : good) rx.feed(b);
    const uint8_t bad[]  = {0x2E, 0x20, 0x02, 0x01, 0x01, 0x00};
    for (uint8_t b : bad) rx.feed(b);

    // A NACK invites a retransmit; silence walks toward the three-strike clause.
    // Blanket ack is the only policy that can trip neither.
    TEST_ASSERT_EQUAL_INT(2, acks);
}

void test_disabled_sink_writes_nothing()
{
    g_writes = 0;
    Sinks::CanboxUartSink s;
    s.begin(capture, nullptr);
    s.setEnabled(false);

    Keys::Event e;
    e.action = Keys::Action::VolUp;
    e.state  = Keys::State::Press;
    s.onKey(e);
    TEST_ASSERT_EQUAL_INT(0, g_writes);
}

// Every frame, in order, as (code, state).
static uint8_t g_seqCode[16], g_seqState[16]; static int g_seqN;
static bool    g_refuse = false;
static uint32_t g_now = 0;
static void captureSeq(const uint8_t* f, uint8_t, void*)
{
    if (g_seqN < 16) { g_seqCode[g_seqN] = f[3]; g_seqState[g_seqN] = f[4]; ++g_seqN; }
}
static bool captureOk(const uint8_t* f, uint8_t n, void* ctx)
{
    if (g_refuse) return false;
    captureSeq(f, n, ctx);
    return true;
}
static Sinks::CanboxUartSink seqSink()
{
    g_seqN = 0; g_refuse = false; g_now = 0;
    Sinks::CanboxUartSink s;
    s.begin(captureSeq, nullptr);
    s.setClock([]() -> uint32_t { return g_now; });
    return s;
}
static Keys::Event key(Keys::Action a, Keys::State st, uint32_t t, uint8_t code = 0)
{
    Keys::Event e; e.action = a; e.state = st; e.tMs = t; e.codeHint = code;
    return e;
}

// A Tap, a panel key, a macro's Action: the release arrives right behind its
// press. It leaves MIN_GAP_MS after the press, not 1.5 ms after it.
void test_a_release_on_the_heels_of_its_press_is_scheduled()
{
    Sinks::CanboxUartSink s = seqSink();
    g_now = 1000;
    s.onKey(key(Keys::Action::VolUp, Keys::State::Press, 1000));
    g_now = 1001;
    s.onKey(key(Keys::Action::VolUp, Keys::State::Release, 1000));
    TEST_ASSERT_EQUAL_INT(1, g_seqN);
    uint32_t due = 0;
    TEST_ASSERT_TRUE(s.nextDue(due));
    TEST_ASSERT_EQUAL_UINT32(1000 + Keys::MIN_GAP_MS, due);
    s.tick(due - 1);
    TEST_ASSERT_EQUAL_INT(1, g_seqN);
    s.tick(due);
    TEST_ASSERT_EQUAL_INT(2, g_seqN);
    TEST_ASSERT_EQUAL_UINT8(Canbox::KEY_RELEASE, g_seqState[1]);

    // A key held longer than the gap releases at once.
    g_now = 2000;
    s.onKey(key(Keys::Action::VolUp, Keys::State::Press, 2000));
    g_now = 2300;
    s.onKey(key(Keys::Action::VolUp, Keys::State::Release, 2300));
    TEST_ASSERT_EQUAL_INT(4, g_seqN);
    TEST_ASSERT_FALSE(s.nextDue(due));
}

// Turning the link off inside a detent's pulse must not strand its release.
void test_switching_the_link_off_sends_the_scheduled_release()
{
    Sinks::CanboxUartSink s = seqSink();
    g_now = 1000;
    s.onKey(key(Keys::Action::EncCw, Keys::State::Press, 1000, Canbox::RAV4_ENTER));
    TEST_ASSERT_EQUAL_INT(1, g_seqN);
    s.setEnabled(false);
    TEST_ASSERT_EQUAL_INT(2, g_seqN);
    TEST_ASSERT_EQUAL_UINT8(Canbox::KEY_RELEASE, g_seqState[1]);
    uint32_t due = 0;
    TEST_ASSERT_FALSE(s.nextDue(due));
    s.tick(5000);
    TEST_ASSERT_EQUAL_INT(2, g_seqN);
}

// Detents on a plain code faster than the pulse: each press is preceded by
// the previous one's release, never press-press-release.
void test_a_second_detent_on_one_code_releases_the_first()
{
    Sinks::CanboxUartSink s = seqSink();
    g_now = 1000;
    s.onKey(key(Keys::Action::EncCw, Keys::State::Press, 1000, Canbox::RAV4_UP));
    g_now = 1010;
    s.onKey(key(Keys::Action::EncCw, Keys::State::Press, 1010, Canbox::RAV4_UP));
    TEST_ASSERT_EQUAL_INT(3, g_seqN);
    TEST_ASSERT_EQUAL_UINT8(Canbox::KEY_PRESS,   g_seqState[0]);
    TEST_ASSERT_EQUAL_UINT8(Canbox::KEY_RELEASE, g_seqState[1]);
    TEST_ASSERT_EQUAL_UINT8(Canbox::KEY_PRESS,   g_seqState[2]);
    s.tick(1010 + 60);
    TEST_ASSERT_EQUAL_INT(4, g_seqN);
    TEST_ASSERT_EQUAL_UINT8(Canbox::KEY_RELEASE, g_seqState[3]);
}

// A release the writer refused is tried again, and only a taken frame counts.
void test_a_refused_release_is_tried_again()
{
    Sinks::CanboxUartSink s = seqSink();
    s.begin(captureOk, nullptr);
    g_now = 1000;
    s.onKey(key(Keys::Action::EncCw, Keys::State::Press, 1000, Canbox::RAV4_ENTER));
    TEST_ASSERT_EQUAL_UINT32(1, s.framesSent());
    g_refuse = true;
    s.tick(1060);
    uint32_t due = 0;
    TEST_ASSERT_TRUE(s.nextDue(due));
    TEST_ASSERT_EQUAL_UINT32(1, s.framesSent());
    g_refuse = false;
    s.tick(1061);
    TEST_ASSERT_EQUAL_UINT32(2, s.framesSent());
    TEST_ASSERT_EQUAL_UINT8(Canbox::KEY_RELEASE, g_seqState[1]);
    TEST_ASSERT_FALSE(s.nextDue(due));
}

// A map row with asLong makes Mute a Long, not a Press: still SOURCE held.
// A Long after the Press that already held it is not sent twice.
void test_mute_as_long_is_sent_once()
{
    Sinks::CanboxUartSink s = seqSink();
    g_now = 1000;
    s.onKey(key(Keys::Action::Mute, Keys::State::Long, 1000));
    TEST_ASSERT_EQUAL_INT(1, g_seqN);
    TEST_ASSERT_EQUAL_UINT8(Canbox::RAV4_SOURCE, g_seqCode[0]);
    TEST_ASSERT_EQUAL_UINT8(Canbox::KEY_LONG, g_seqState[0]);
    g_now = 1500;
    s.onKey(key(Keys::Action::Mute, Keys::State::Release, 1500));
    TEST_ASSERT_EQUAL_INT(2, g_seqN);

    g_now = 3000;
    s.onKey(key(Keys::Action::Mute, Keys::State::Press, 3000));
    s.onKey(key(Keys::Action::Mute, Keys::State::Long, 3600));
    TEST_ASSERT_EQUAL_INT(3, g_seqN);
}

// A head unit that stops mid-frame must not swallow the frames after it.
void test_rx_reset_abandons_a_truncated_frame()
{
    static int frames = 0;
    frames = 0;
    Canbox::RxParser rx;
    rx.onFrame([](uint8_t, const uint8_t*, uint8_t, void*) { frames++; }, nullptr);

    const uint8_t cut[] = {0x2E, 0x20, 0x05, 0x01};     // promises 5, sends 1
    for (uint8_t b : cut) rx.feed(b);
    TEST_ASSERT_TRUE(rx.midFrame());
    rx.reset();
    TEST_ASSERT_FALSE(rx.midFrame());
    TEST_ASSERT_EQUAL_UINT32(1, rx.desyncs());
    rx.reset();                                          // idle: nothing to count
    TEST_ASSERT_EQUAL_UINT32(1, rx.desyncs());

    const uint8_t good[] = {0x2E, 0x20, 0x02, 0x01, 0x01, 0xDB};
    for (uint8_t b : good) rx.feed(b);
    TEST_ASSERT_EQUAL_INT(1, frames);
}

// ---- the catalogue ----------------------------------------------------------
// It is a table of claims about a protocol, and the failure mode of a table of
// claims is that one row quietly stops matching the code it describes. These
// checks are cheap and they catch exactly that.

void test_the_key_table_has_no_duplicate_codes()
{
    for (uint8_t i = 0; i < Canbox::keyCodeCount(); ++i) {
        const Canbox::KeyCode& a = Canbox::keyCodeAt(i);
        TEST_ASSERT_NOT_NULL(a.name);
        TEST_ASSERT_TRUE(a.name[0] != '\0');
        for (uint8_t j = (uint8_t)(i + 1); j < Canbox::keyCodeCount(); ++j)
            TEST_ASSERT_TRUE(a.code != Canbox::keyCodeAt(j).code);
    }
}

// Every code the default action mapping can produce must be one the catalogue
// can name, or the mapping UI shows a byte with no explanation next to it.
void test_every_profile_default_code_is_in_the_catalogue()
{
    for (uint8_t i = 0; i < (uint8_t)Keys::Action::_Count; ++i) {
        const uint8_t code = Canbox::rav4CodeFor((Keys::Action)i);
        if (!code) continue;             // 0 = this action has no code here
        TEST_ASSERT_TRUE(strcmp(Canbox::keyCodeName(code), "?") != 0);
    }
}

void test_every_message_carries_a_confidence_and_a_note()
{
    for (uint8_t i = 0; i < Canbox::Rav4Emitter::messageCount(); ++i) {
        const Canbox::MsgInfo& m = Canbox::msgInfo(i);
        TEST_ASSERT_NOT_NULL(m.note);
        TEST_ASSERT_TRUE(m.note[0] != '\0');
        TEST_ASSERT_TRUE(strcmp(Canbox::confName(m.conf), "?") != 0);
    }
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_frame_golden_vector);
    RUN_TEST(test_checksum_is_sum_xor_ff);
    RUN_TEST(test_oversized_payload_is_refused);
    RUN_TEST(test_rx_parses_frame_after_junk);
    RUN_TEST(test_rx_survives_a_frame_longer_than_the_buffer);
    RUN_TEST(test_rx_keeps_a_long_text_payload_readable);
    RUN_TEST(test_rx_rejects_bad_checksum);
    RUN_TEST(test_defaults_match_the_megane_keypad);
    RUN_TEST(test_keymap_blob_roundtrip);
    RUN_TEST(test_blob_with_unknown_action_is_rejected);
    RUN_TEST(test_encoder_on_a_plain_code_is_a_scheduled_pulse);
    RUN_TEST(test_code_override_beats_the_profile_default);
    RUN_TEST(test_wheel_sends_a_detent_count_and_no_release);
    RUN_TEST(test_mute_is_a_long_press_of_source);
    RUN_TEST(test_rx_acks_every_frame_including_a_corrupt_one);
    RUN_TEST(test_disabled_sink_writes_nothing);
    RUN_TEST(test_a_release_on_the_heels_of_its_press_is_scheduled);
    RUN_TEST(test_switching_the_link_off_sends_the_scheduled_release);
    RUN_TEST(test_a_second_detent_on_one_code_releases_the_first);
    RUN_TEST(test_a_refused_release_is_tried_again);
    RUN_TEST(test_mute_as_long_is_sent_once);
    RUN_TEST(test_rx_reset_abandons_a_truncated_frame);
    RUN_TEST(test_the_key_table_has_no_duplicate_codes);
    RUN_TEST(test_every_profile_default_code_is_in_the_catalogue);
    RUN_TEST(test_every_message_carries_a_confidence_and_a_note);
    return UNITY_END();
}
