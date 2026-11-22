#include <unity.h>
#include "net/NetBudget.h"

// The heap budget a network job asks (src/net/NetBudget.h): free internal
// heap against the path's need plus the reserve, and the largest block
// against the path's biggest single allocation, never more.

void setUp() {}
void tearDown() {}

using NetBudget::Path;
using NetBudget::verdict;
using NetBudget::needFree;

void test_wifi_needs_the_tls_side_plus_the_reserve()
{
    TEST_ASSERT_EQUAL_UINT32(20 * 1024, needFree(Path::Wifi));
    TEST_ASSERT_TRUE(verdict(Path::Wifi, 20 * 1024, 10 * 1024));
    TEST_ASSERT_FALSE(verdict(Path::Wifi, 20 * 1024 - 1, 10 * 1024));
}

void test_the_app_path_asks_less()
{
    TEST_ASSERT_TRUE(needFree(Path::App) < needFree(Path::Wifi));
    TEST_ASSERT_TRUE(verdict(Path::App, 12 * 1024, 4 * 1024));
    TEST_ASSERT_FALSE(verdict(Path::Wifi, 12 * 1024, 4 * 1024));
    TEST_ASSERT_FALSE(verdict(Path::App, needFree(Path::App) - 1, 4 * 1024));
}

// The car's normal: plenty free, the largest block ~10 kB. Admitted.
void test_a_10k_largest_block_is_normal()
{
    TEST_ASSERT_TRUE(verdict(Path::Wifi, 60 * 1024, 10 * 1024));
    TEST_ASSERT_TRUE(verdict(Path::Wifi, 60 * 1024, NetBudget::BIG_WIFI));
    TEST_ASSERT_TRUE(NetBudget::BIG_WIFI <= NetBudget::NEED_WIFI);
    TEST_ASSERT_TRUE(NetBudget::BIG_APP <= NetBudget::NEED_APP);
}

// Boot 292's dip: min=4752 big=1524. Held on both paths.
void test_the_car_dip_is_held()
{
    TEST_ASSERT_FALSE(verdict(Path::Wifi, 4752, 1524));
    TEST_ASSERT_FALSE(verdict(Path::App, 4752, 1524));
}

// Enough free but fragmented below one WiFi RX buffer: held.
void test_a_fragmented_heap_is_held()
{
    TEST_ASSERT_FALSE(verdict(Path::Wifi, 40 * 1024, NetBudget::BIG_WIFI - 1));
    TEST_ASSERT_FALSE(verdict(Path::App, 40 * 1024, NetBudget::BIG_APP - 1));
    TEST_ASSERT_TRUE(verdict(Path::App, 40 * 1024, NetBudget::BIG_APP));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_wifi_needs_the_tls_side_plus_the_reserve);
    RUN_TEST(test_the_app_path_asks_less);
    RUN_TEST(test_a_10k_largest_block_is_normal);
    RUN_TEST(test_the_car_dip_is_held);
    RUN_TEST(test_a_fragmented_heap_is_held);
    return UNITY_END();
}
