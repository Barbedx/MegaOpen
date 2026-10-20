#include <unity.h>
#include <mutex>
#include <thread>
#include <vector>
#include "display/GlassGate.h"

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

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_repaint_with_the_current_epoch_is_taken);
    RUN_TEST(test_a_persons_screen_moves_the_seq_and_a_stale_repaint_yields);
    RUN_TEST(test_a_refused_persons_screen_does_not_hold_the_glass);
    RUN_TEST(test_neither_kind_does_not_take_the_lock);
    RUN_TEST(test_a_screen_and_a_repaint_never_interleave);
    return UNITY_END();
}
