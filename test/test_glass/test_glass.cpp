#include <unity.h>
#include <string.h>
#include <mutex>
#include <thread>
#include <vector>
#include "display/GlassGate.h"
#include "display/PopupGate.h"
#include "display/InfoRaw.h"

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
    return UNITY_END();
}
