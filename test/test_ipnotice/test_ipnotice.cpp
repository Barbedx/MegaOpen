#include <unity.h>
#include "net/IpNotice.h"

// When the board's address goes on the panel (src/net/IpNotice.h): once per
// connection. Too often and every WiFi flap covers the screen; too rarely and
// the owner has a board on a new network and no address for it.

void setUp() {}
void tearDown() {}

using Mode = IpNotice::Mode;
static constexpr uint32_t F    = IpNotice::FLAP_MS;
static constexpr uint32_t HOME = 0xC0A8010Fu;   // 192.168.1.15
static constexpr uint32_t AP   = 0xC0A80401u;   // 192.168.4.1

// Passes every 100 ms from `from` to `to` (inclusive) with one link: how many
// notices, and the time of the first.
static int run(IpNotice& n, uint32_t from, uint32_t to, Mode m, uint32_t ip, uint32_t* at = nullptr)
{
    int k = 0;
    if (at) *at = 0;
    for (uint32_t t = from; t <= to; t += 100)
        if (n.tick(t, m, ip)) { if (!k && at) *at = t; ++k; }
    return k;
}

void test_the_first_link_of_a_boot_is_announced_once()
{
    IpNotice n;
    uint32_t at = 0;
    TEST_ASSERT_EQUAL(0, run(n, 0, 5000, Mode::None, 0));          // joining
    TEST_ASSERT_EQUAL(1, run(n, 5100, 120000, Mode::Sta, HOME, &at));
    TEST_ASSERT_EQUAL_UINT32(5100, at);
}

void test_a_flap_under_30_s_with_the_same_address_is_silent()
{
    IpNotice n;
    run(n, 0, 1000, Mode::Sta, HOME);
    TEST_ASSERT_EQUAL(0, run(n, 1100, 1100 + F - 200, Mode::None, 0));
    TEST_ASSERT_EQUAL(0, run(n, 1100 + F - 100, 60000, Mode::Sta, HOME));
}

void test_repeated_short_flaps_never_announce()
{
    IpNotice n;
    run(n, 0, 1000, Mode::Sta, HOME);
    uint32_t t = 1100;
    for (int i = 0; i < 10; ++i) {
        TEST_ASSERT_EQUAL(0, run(n, t, t + 10000, Mode::None, 0));
        TEST_ASSERT_EQUAL(0, run(n, t + 10100, t + 15000, Mode::Sta, HOME));
        t += 15100;
    }
}

void test_down_for_30_s_or_more_is_a_new_connection()
{
    IpNotice n;
    run(n, 0, 1000, Mode::Sta, HOME);
    run(n, 1100, 1100 + F - 100, Mode::None, 0);                   // exactly FLAP_MS down
    uint32_t at = 0;
    TEST_ASSERT_EQUAL(1, run(n, 1100 + F, 1100 + F + 5000, Mode::Sta, HOME, &at));
    TEST_ASSERT_EQUAL_UINT32(1100 + F, at);
}

void test_a_new_address_is_announced_even_after_a_short_flap()
{
    IpNotice n;
    run(n, 0, 1000, Mode::Sta, HOME);
    run(n, 1100, 3000, Mode::None, 0);
    TEST_ASSERT_EQUAL(1, run(n, 3100, 9000, Mode::Sta, HOME + 1));
}

void test_a_new_lease_without_a_down_pass_is_announced()
{
    IpNotice n;
    run(n, 0, 1000, Mode::Sta, HOME);
    TEST_ASSERT_EQUAL(1, run(n, 1100, 5000, Mode::Sta, HOME + 7));
}

void test_the_ap_after_the_home_network_is_lost_is_announced()
{
    // WifiManager: STA gone 30 s, then the AP comes up.
    IpNotice n;
    run(n, 0, 1000, Mode::Sta, HOME);
    run(n, 1100, 31100, Mode::None, 0);
    TEST_ASSERT_EQUAL(1, run(n, 31200, 40000, Mode::Ap, AP));
}

void test_the_ap_retry_every_minute_is_silent()
{
    // In AP mode the board tries the saved networks every 60 s: ~8 s of a
    // join (None), then the same AP again.
    IpNotice n;
    TEST_ASSERT_EQUAL(1, run(n, 0, 60000, Mode::Ap, AP));
    for (uint32_t t = 60100; t < 600000; t += 68200) {
        TEST_ASSERT_EQUAL(0, run(n, t, t + 8000, Mode::None, 0));
        TEST_ASSERT_EQUAL(0, run(n, t + 8100, t + 68100, Mode::Ap, AP));
    }
}

void test_the_home_network_after_the_ap_is_announced_at_once()
{
    IpNotice n;
    run(n, 0, 5000, Mode::Ap, AP);
    run(n, 5100, 13000, Mode::None, 0);                            // the retry's join
    TEST_ASSERT_EQUAL(1, run(n, 13100, 20000, Mode::Sta, HOME));
}

void test_the_clock_wrapping_does_not_announce_a_flap()
{
    IpNotice n;
    const uint32_t t0 = 0xFFFFFFFFu - 5000;
    TEST_ASSERT_TRUE(n.tick(t0, Mode::Sta, HOME));
    TEST_ASSERT_FALSE(n.tick(t0 + 1000, Mode::None, 0));
    TEST_ASSERT_FALSE(n.tick(t0 + 11000, Mode::Sta, HOME));        // wrapped, 10 s down
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_first_link_of_a_boot_is_announced_once);
    RUN_TEST(test_a_flap_under_30_s_with_the_same_address_is_silent);
    RUN_TEST(test_repeated_short_flaps_never_announce);
    RUN_TEST(test_down_for_30_s_or_more_is_a_new_connection);
    RUN_TEST(test_a_new_address_is_announced_even_after_a_short_flap);
    RUN_TEST(test_a_new_lease_without_a_down_pass_is_announced);
    RUN_TEST(test_the_ap_after_the_home_network_is_lost_is_announced);
    RUN_TEST(test_the_ap_retry_every_minute_is_silent);
    RUN_TEST(test_the_home_network_after_the_ap_is_announced_at_once);
    RUN_TEST(test_the_clock_wrapping_does_not_announce_a_flap);
    return UNITY_END();
}
