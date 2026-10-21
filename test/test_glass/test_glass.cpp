#include <unity.h>
#include <string.h>
#include <mutex>
#include <thread>
#include <vector>
#include "display/GlassGate.h"
#include "display/PopupGate.h"
#include "display/InfoRaw.h"
#include <stdio.h>
#include <initializer_list>
#include <AffaDisplay.h>
#include "link/LoopbackLink.h"

using namespace MegaOpen;

void setUp() {}
void tearDown() {}

// The glass check (src/display/GlassGate.h): a repaint decided before a
// person's screen was taken must never be taken after it. The library admits
// renders in the order they were taken, so that order is what is checked.

struct StdLock {
    std::mutex m;
    bool lock()   { m.lock(); return true; }
    void unlock() { m.unlock(); }
};

struct NeverLock {
    bool lock()   { return false; }
    void unlock() {}
};

static void test_a_repaint_with_the_current_epoch_is_taken()
{
    GlassGate<StdLock> g;
    int calls = 0;
    TEST_ASSERT_EQUAL((int)GlassVerdict::Taken,
                      (int)g.run(true, g.seq(), false, [&]() { ++calls; return true; }));
    TEST_ASSERT_EQUAL(1, calls);
    TEST_ASSERT_EQUAL_UINT16(0, g.seq());
}

static void test_a_persons_screen_moves_the_seq_and_a_stale_repaint_yields()
{
    GlassGate<StdLock> g;
    const uint16_t epoch = g.seq();
    TEST_ASSERT_EQUAL((int)GlassVerdict::Taken, (int)g.run(false, 0, true, []() { return true; }));
    TEST_ASSERT_EQUAL_UINT16(1, g.seq());
    int calls = 0;
    TEST_ASSERT_EQUAL((int)GlassVerdict::Yielded,
                      (int)g.run(true, epoch, false, [&]() { ++calls; return true; }));
    TEST_ASSERT_EQUAL(0, calls);
    TEST_ASSERT_EQUAL_UINT32(1, g.yielded());
}

static void test_a_refused_persons_screen_does_not_hold_the_glass()
{
    GlassGate<StdLock> g;
    TEST_ASSERT_EQUAL((int)GlassVerdict::Refused, (int)g.run(false, 0, true, []() { return false; }));
    TEST_ASSERT_EQUAL_UINT16(0, g.seq());
}

static void test_neither_kind_does_not_take_the_lock()
{
    GlassGate<NeverLock> g;
    TEST_ASSERT_EQUAL((int)GlassVerdict::Taken, (int)g.run(false, 0, false, []() { return true; }));
    TEST_ASSERT_EQUAL((int)GlassVerdict::Busy, (int)g.run(false, 0, true, []() { return true; }));
    TEST_ASSERT_EQUAL((int)GlassVerdict::Busy, (int)g.run(true, 0, false, []() { return true; }));
}

// One thread takes person's screens, another repaints on the seq it last saw.
// Every repaint taken must carry exactly the number of person's screens taken
// before it: one taken after a newer screen is the glass covered.
static void test_a_screen_and_a_repaint_never_interleave()
{
    GlassGate<StdLock> g;
    struct Entry { bool deliberate; uint16_t epoch; };
    std::vector<Entry> taken;
    taken.reserve(40000);
    constexpr int SCREENS = 3000;
    std::atomic<bool> done{false};

    std::thread person([&]() {
        for (int i = 0; i < SCREENS; ++i)
            (void)g.run(false, 0, true, [&]() { taken.push_back({true, 0}); return true; });
        done = true;
    });
    std::thread ui([&]() {
        while (!done) {
            const uint16_t epoch = g.seq();
            std::this_thread::yield();
            (void)g.run(true, epoch, false, [&]() { taken.push_back({false, epoch}); return true; });
        }
    });
    person.join();
    ui.join();

    uint16_t screens = 0;
    for (const Entry& e : taken) {
        if (e.deliberate) { ++screens; continue; }
        TEST_ASSERT_EQUAL_UINT16(screens, e.epoch);
    }
    TEST_ASSERT_EQUAL_UINT16(SCREENS, screens);
    TEST_ASSERT_EQUAL_UINT16(SCREENS, g.seq());
}

// ---- the popup gate (src/display/PopupGate.h) --------------------------------

void test_popup_gate_holds_until_the_deadline()
{
    TEST_ASSERT_FALSE(popupHolds(0, 1000));                 // no popup
    TEST_ASSERT_TRUE(popupHolds(3000, 1000));               // live
    TEST_ASSERT_TRUE(popupHolds(3000, 2999));
    TEST_ASSERT_FALSE(popupHolds(3000, 3000));              // its deadline: released
    TEST_ASSERT_FALSE(popupHolds(3000, 9000));              // past it, take-down or not
    // Across the millis() wrap.
    TEST_ASSERT_TRUE(popupHolds(0x00000100u, 0xFFFFFF00u));
    TEST_ASSERT_FALSE(popupHolds(0xFFFFFF00u, 0x00000100u));
}

// The ui task's shape, in miniature: a screen that changed while a popup was
// up is not sent under it, stays owed, and goes on the first pass at the
// deadline; one pass is 50 ms.
void test_a_repaint_under_a_popup_is_held_then_sent()
{
    uint32_t popupUntil = 0;
    bool dirty = false;
    std::vector<uint32_t> sent;
    auto pass = [&](uint32_t now) {
        if (dirty && !popupHolds(popupUntil, now)) { sent.push_back(now); dirty = false; }
        if (popupUntil && (int32_t)(now - popupUntil) >= 0) popupUntil = 0;   // take-down
    };
    popupUntil = 1000 + 2000;                               // a volume popup at t = 1000, 2 s
    for (uint32_t t = 1000; t < 2000; t += 50) pass(t);
    dirty = true;                                           // the track changes at 2000
    for (uint32_t t = 2000; t < 4000; t += 50) pass(t);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)sent.size());
    TEST_ASSERT_EQUAL_UINT32(3000, sent[0]);                // the deadline's pass, not before
    dirty = true;                                           // no popup now: at once
    pass(4000);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)sent.size());
    TEST_ASSERT_EQUAL_UINT32(4000, sent[1]);
}

// ---- the dev-only info row (src/display/InfoRaw.h) ------------------------------

void test_inforaw_at_0x0B_is_the_librarys_row()
{
    uint8_t f[InfoRaw::FRAME_MAX];
    // showInfoMenu's own form: 10 0B 76 60 41 + 8 space-padded cells.
    const uint8_t want[] = {0x10, 0x0B, 0x76, 0x60, 0x41, 'A', 'U', 'X', ' ', ' ', ' ', ' ', ' '};
    TEST_ASSERT_EQUAL_UINT32(sizeof(want), InfoRaw::build(f, 0x0B, 0x60, 0x41, "AUX"));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f, sizeof(want));
}

void test_inforaw_longer_rows_and_bounds()
{
    uint8_t f[InfoRaw::FRAME_MAX + 4];
    memset(f, 0xEE, sizeof(f));
    // 0x13: 16 cells, the text cut to them; nothing written past FRAME_MAX.
    TEST_ASSERT_EQUAL_UINT32(2 + 0x13, InfoRaw::build(f, 0x13, 0x60, 0x44, "123456789ABCDEFGHIJK"));
    TEST_ASSERT_EQUAL_HEX8(0x13, f[1]);
    TEST_ASSERT_EQUAL_HEX8(0x44, f[4]);
    TEST_ASSERT_EQUAL_MEMORY("123456789ABCDEFG", f + 5, 16);
    TEST_ASSERT_EQUAL_HEX8(0xEE, f[InfoRaw::FRAME_MAX]);
    // A length is clamped by the route; build() itself refuses what is out.
    TEST_ASSERT_EQUAL_UINT8(0x0B, InfoRaw::clampLen(0));
    TEST_ASSERT_EQUAL_UINT8(0x13, InfoRaw::clampLen(0xFF));
    TEST_ASSERT_EQUAL_UINT8(0x0E, InfoRaw::clampLen(0x0E));
    TEST_ASSERT_EQUAL_UINT32(0, InfoRaw::build(f, 0x0A, 0x60, 0x41, "x"));
    TEST_ASSERT_EQUAL_UINT32(0, InfoRaw::build(f, 0x14, 0x60, 0x41, "x"));
    TEST_ASSERT_EQUAL_UINT32(0, InfoRaw::build(f, 0x0B, 0x60, 0x50, "x"));    // offset
    TEST_ASSERT_EQUAL_UINT32(0, InfoRaw::build(f, 0x0B, 0x3F, 0x41, "x"));    // prefix
    TEST_ASSERT_EQUAL_UINT32(2 + 0x0B, InfoRaw::build(f, 0x0B, 0x60, 0x48, nullptr));
    TEST_ASSERT_EQUAL_HEX8(' ', f[5]);
}

// ---- the library's showInfoRow against InfoRaw.h --------------------------------
// The dev route moves from InfoRaw::build + enqueue(0x151) to the library's
// showInfoRow(text, offset, prefix, len - 3). Proof that the bytes on the wire are
// the same: a real CarminatDisplay over a loopback link, a session opened the way
// the panel opens it, then the one row reassembled from its ISO-TP frames.

namespace
{
    struct Clock final : affa::IClock {
        uint32_t t = 0;
        uint32_t millis() const override { return t; }
    };

    affa::Frame frame(uint32_t id, std::initializer_list<uint8_t> b)
    {
        affa::Frame f;
        f.id = id;
        f.len = 8;
        uint8_t i = 0;
        for (uint8_t v : b) f.data[i++] = v;
        return f;
    }

    // The opening as the panel drives it (AffaDisplay's test/affa_test_support.h):
    // two sync requests, the hello burst with the panel's own 1C1 registration
    // between B0#1 and B0#2, our registrations, the quiet period, the power-on.
    template <class L>
    void openSession(affa::CarminatDisplay& d, L& link, Clock& clk)
    {
        d.begin();
        d.setSelfAck(true);
        const affa::Frame req = frame(0x3CF, {0x61, 0x11, 0x00, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3});
        link.inject(req); d.poll();
        link.inject(req); d.poll();
        clk.t += affa::carminat::kHelloFirstDelayMs; d.poll();
        link.inject(frame(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
        clk.t += affa::carminat::kHelloFrameGapMs; d.poll();
        clk.t += affa::carminat::kHelloFrameGapMs; d.poll();
        for (int i = 0; i < 16 && !d.registered(); ++i) d.poll();
        TEST_ASSERT_TRUE_MESSAGE(d.registered(), "the session opens");
        clk.t += affa::carminat::kPayloadAfterRegistrationMs;
        for (int i = 0; i < 64 && (d.busy() || d.phase() != affa::Phase::Ready); ++i) d.poll();
        TEST_ASSERT_TRUE_MESSAGE(d.phase() == affa::Phase::Ready, "and reaches Ready");
        affa::Frame f;
        while (link.takeSent(f)) {}
    }

    // The 0x151 message on the wire as InfoRaw::build lays it out: 10 <len> + content.
    template <class L>
    size_t reassemble(L& link, uint8_t* out, size_t max)
    {
        affa::Frame f;
        size_t n = 0, want = 0;
        while (link.takeSent(f)) {
            if (f.id != 0x151) continue;
            if ((f.data[0] & 0xF0) == 0x10) {
                want = 2 + f.data[1];
                for (uint8_t i = 0; i < 8 && n < want && n < max; ++i) out[n++] = f.data[i];
            } else if ((f.data[0] & 0xF0) == 0x20) {
                for (uint8_t i = 1; i < 8 && n < want && n < max; ++i) out[n++] = f.data[i];
            }
        }
        return n;
    }
}

void test_library_info_row_is_inforaws_bytes()
{
    struct Case { uint8_t len, prefix, offset; const char* text; };
    const Case cases[] = {
        {0x0B, 0x60, 0x41, "AUX"},                    // the library's own 8 cells
        {0x0F, 0x60, 0x44, "123456789ABCDEFG"},       // 12 cells, the ruler cut
        {0x13, 0x60, 0x48, "123456789ABCDEFGHIJK"},   // 16 cells, the most
        {0x13, 0x50, 0x40, "AB"},                     // other prefix and offset, padded
    };
    for (const Case& c : cases) {
        uint8_t want[InfoRaw::FRAME_MAX];
        const size_t wn = InfoRaw::build(want, c.len, c.prefix, c.offset, c.text);
        TEST_ASSERT_NOT_EQUAL(0, wn);

        affa::LoopbackLink<256> link;
        Clock clk;
        affa::CarminatDisplay d(link, clk);
        openSession(d, link, clk);
        TEST_ASSERT_TRUE(d.showInfoRow(c.text, c.offset, c.prefix, (uint8_t)(c.len - 3)).result ==
                         affa::Result::Ok);
        for (int i = 0; i < 64 && d.busy(); ++i) d.poll();

        uint8_t got[64];
        const size_t gn = reassemble(link, got, sizeof(got));
        char msg[32];
        snprintf(msg, sizeof(msg), "len 0x%02X", (unsigned)c.len);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(wn, gn, msg);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, got, wn, msg);
    }
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_repaint_with_the_current_epoch_is_taken);
    RUN_TEST(test_a_persons_screen_moves_the_seq_and_a_stale_repaint_yields);
    RUN_TEST(test_a_refused_persons_screen_does_not_hold_the_glass);
    RUN_TEST(test_neither_kind_does_not_take_the_lock);
    RUN_TEST(test_a_screen_and_a_repaint_never_interleave);
    RUN_TEST(test_popup_gate_holds_until_the_deadline);
    RUN_TEST(test_a_repaint_under_a_popup_is_held_then_sent);
    RUN_TEST(test_inforaw_at_0x0B_is_the_librarys_row);
    RUN_TEST(test_inforaw_longer_rows_and_bounds);
    RUN_TEST(test_library_info_row_is_inforaws_bytes);
    return UNITY_END();
}
