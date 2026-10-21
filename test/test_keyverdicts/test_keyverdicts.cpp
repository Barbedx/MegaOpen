#include <unity.h>
#include <atomic>
#include <thread>
#include "display/KeyVerdicts.h"

using namespace MegaOpen;

void setUp() {}
void tearDown() {}

// A posted key's verdict (src/display/KeyVerdicts.h): reported exactly once,
// whichever side gets there first, and never mistaken for a render's.

static void test_a_verdict_after_expect_is_the_owners_to_report()
{
    KeyVerdicts k;
    uint8_t r = 0xEE;
    TEST_ASSERT_FALSE(k.expect(7, r));
    TEST_ASSERT_TRUE(k.complete(7, 9));
    // Once: a second completion of the same ticket is nobody's.
    TEST_ASSERT_FALSE(k.complete(7, 9));
}

static void test_a_verdict_before_expect_is_the_posters_to_report()
{
    KeyVerdicts k;
    TEST_ASSERT_FALSE(k.complete(8, 1));     // nobody waits yet: a render's, as far as it knows
    uint8_t r = 0;
    TEST_ASSERT_TRUE(k.expect(8, r));
    TEST_ASSERT_EQUAL_UINT8(1, r);
}

static void test_a_renders_ticket_is_not_a_key()
{
    KeyVerdicts k;
    uint8_t r = 0;
    TEST_ASSERT_FALSE(k.expect(9, r));
    TEST_ASSERT_FALSE(k.complete(10, 0));
    TEST_ASSERT_TRUE(k.complete(9, 0));
}

static void test_no_ticket_is_never_a_key()
{
    KeyVerdicts k;
    uint8_t r = 0;
    TEST_ASSERT_FALSE(k.expect(0, r));
    TEST_ASSERT_FALSE(k.complete(0, 0));
}

// Poster and owner on two threads, the owner sometimes first: every ticket is
// reported exactly once. One key in flight at a time (a person at a web page
// posts no faster), so the slots are never all taken.
static void test_racing_poster_and_owner_report_each_key_once()
{
    constexpr uint16_t KEYS = 20000;
    KeyVerdicts k;
    std::atomic<uint16_t> posted{0}, completed{0};
    std::atomic<uint32_t> byPoster{0}, byOwner{0};

    std::thread owner([&]() {
        for (uint16_t t = 1; t <= KEYS; ++t) {
            while (posted.load() < t) {}
            if (k.complete(t, 3)) byOwner.fetch_add(1);
            completed.store(t);
        }
    });
    std::thread poster([&]() {
        for (uint16_t t = 1; t <= KEYS; ++t) {
            while (completed.load() + 1 < t) {}
            posted.store(t);                     // the library has the key now
            if (t & 1) std::this_thread::yield();
            uint8_t r = 0;
            if (k.expect(t, r)) {
                TEST_ASSERT_EQUAL_UINT8(3, r);
                byPoster.fetch_add(1);
            }
        }
    });
    poster.join();
    owner.join();
    TEST_ASSERT_EQUAL_UINT32(KEYS, byPoster.load() + byOwner.load());
}

// More keys in flight than slots: the extra one is said to be unwatched (and
// counted), not dropped silently; one whose verdict came early still reports.
static void test_a_key_past_the_slots_is_flagged_unwatched()
{
    MegaOpen::KeyVerdicts k;
    uint8_t r = 0;
    for (uint16_t t = 1; t <= MegaOpen::KeyVerdicts::SLOTS; ++t) TEST_ASSERT_FALSE(k.expect(t, r));
    bool unwatched = false;
    TEST_ASSERT_FALSE(k.expect(99, r, &unwatched));
    TEST_ASSERT_TRUE(unwatched);
    TEST_ASSERT_EQUAL_UINT32(1, k.unwatched());
    // A watched one still completes to its poster's slot.
    TEST_ASSERT_TRUE(k.complete(1, 3));
    // Its verdict arrived first: reported, not unwatched, even with every slot busy.
    TEST_ASSERT_FALSE(k.complete(50, 2));
    TEST_ASSERT_FALSE(k.expect(5, r));                 // fills the freed slot
    unwatched = false;
    TEST_ASSERT_TRUE(k.expect(50, r, &unwatched));
    TEST_ASSERT_FALSE(unwatched);
    TEST_ASSERT_EQUAL_UINT8(2, r);
    TEST_ASSERT_EQUAL_UINT32(1, k.unwatched());
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_verdict_after_expect_is_the_owners_to_report);
    RUN_TEST(test_a_verdict_before_expect_is_the_posters_to_report);
    RUN_TEST(test_a_renders_ticket_is_not_a_key);
    RUN_TEST(test_no_ticket_is_never_a_key);
    RUN_TEST(test_racing_poster_and_owner_report_each_key_once);
    RUN_TEST(test_a_key_past_the_slots_is_flagged_unwatched);
    return UNITY_END();
}
