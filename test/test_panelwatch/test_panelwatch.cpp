// Who else is on the panel bus (display/PanelBusWatch.h), on timed frame
// replays. The watch decides nothing (the role does, display/PanelRole.h);
// what it reports — a foreign master, a master gone, a lone cluster — is
// what health `panel` and the [panel] lines say, so it is pinned here.
#include <unity.h>
#include <stdint.h>
#include "display/PanelBusWatch.h"

using MegaOpen::PanelBusWatch;

void setUp() {}
void tearDown() {}

static const uint8_t kSync[]  = {0x79, 0x00};
static const uint8_t kReq[]   = {0x61, 0x11, 0x01};
static const uint8_t kPing[]  = {0x69};
static const uint8_t kOpen[]  = {0x70, 0x00};

static void panelFrames(PanelBusWatch& w, uint32_t at)
{
    w.onRx(0x3CF, 3, kReq, at);
    w.onRx(0x1C1, 2, kOpen, at);
}

// Our bench panel alone: no master, no sync heard, not a cluster.
void test_a_panel_alone_is_no_master()
{
    PanelBusWatch w;
    for (uint32_t t = 5000; t < 9000; t += 100) panelFrames(w, t);
    TEST_ASSERT_FALSE(w.foreignMaster(9000));
    TEST_ASSERT_FALSE(w.heardSync());
    TEST_ASSERT_FALSE(w.clusterOnly());
    TEST_ASSERT_EQUAL_UINT32(0, w.masters());
}

// A stock radio syncing at ~1 s: one sync is heard but is no master yet, the
// second within WINDOW_MS makes it one, and it stays one while it keeps syncing.
void test_a_stock_radio_is_a_master_from_its_second_sync()
{
    PanelBusWatch w;
    w.onRx(0x3AF, 2, kSync, 900);
    TEST_ASSERT_TRUE(w.heardSync());
    TEST_ASSERT_FALSE(w.foreignMaster(900));
    w.onRx(0x3AF, 2, kSync, 1900);
    TEST_ASSERT_TRUE(w.foreignMaster(1900));
    TEST_ASSERT_EQUAL_HEX32(0x3AF, w.masterId());
    for (uint32_t t = 2900; t < 60000; t += 1000) {
        w.onRx(0x3AF, 2, kSync, t);
        TEST_ASSERT_TRUE(w.foreignMaster(t + 500));
    }
    TEST_ASSERT_EQUAL_UINT32(60, w.masters());
}

// Two strays hours apart are not a master: the count is within WINDOW_MS.
void test_strays_hours_apart_are_not_a_master()
{
    PanelBusWatch w;
    w.onRx(0x3DF, 2, kSync, 100000);
    w.onRx(0x3DF, 2, kSync, 100000 + 3600000);
    TEST_ASSERT_FALSE(w.foreignMaster(100000 + 3600000));
    TEST_ASSERT_EQUAL_HEX32(0x3DF, w.masterId());
}

// A master that goes quiet for GONE_MS is gone; a single sync after that
// starts a fresh count instead of re-latching at once.
void test_a_master_quiet_for_the_gone_time_is_gone()
{
    PanelBusWatch w;
    w.onRx(0x3AF, 2, kSync, 1000);
    w.onRx(0x3AF, 2, kSync, 2000);
    TEST_ASSERT_TRUE(w.foreignMaster(2000 + PanelBusWatch::GONE_MS - 1));
    TEST_ASSERT_FALSE(w.foreignMaster(2000 + PanelBusWatch::GONE_MS));
    const uint32_t later = 2000 + PanelBusWatch::GONE_MS + 5000;
    w.onRx(0x3AF, 2, kSync, later);
    TEST_ASSERT_FALSE(w.foreignMaster(later));             // one sync: not re-latched
    w.onRx(0x3AF, 2, kSync, later + 900);
    TEST_ASSERT_TRUE(w.foreignMaster(later + 900));        // a second within the window: master
}

// A lone instrument cluster: 0x3CF DLC 1 0x69 pings, never the panel's 61 11.
void test_a_lone_cluster_reads_as_cluster_only()
{
    PanelBusWatch w;
    w.onRx(0x3CF, 1, kPing, 100);
    w.onRx(0x3CF, 1, kPing, 200);
    TEST_ASSERT_FALSE(w.clusterOnly());
    w.onRx(0x3CF, 1, kPing, 300);
    TEST_ASSERT_TRUE(w.clusterOnly());
    TEST_ASSERT_FALSE(w.foreignMaster(300));
    panelFrames(w, 400);
    TEST_ASSERT_FALSE(w.clusterOnly());
}

// 0x3CF in other shapes counts for neither.
void test_other_3cf_frames_count_for_neither()
{
    PanelBusWatch w;
    const uint8_t a[] = {0x70}, b[] = {0x69, 0x00};
    w.onRx(0x3CF, 1, a, 1);
    w.onRx(0x3CF, 2, b, 2);
    w.onRx(0x3CF, 0, a, 3);
    TEST_ASSERT_EQUAL_UINT32(0, w.clusterPings());
    TEST_ASSERT_EQUAL_UINT32(0, w.panelRequests());
}

// The windows across millis() wrapping.
void test_the_windows_survive_a_millis_wrap()
{
    PanelBusWatch w;
    w.onRx(0x3AF, 2, kSync, 0xFFFFFFF0u);
    w.onRx(0x3AF, 2, kSync, 0x00000300u);
    TEST_ASSERT_TRUE(w.foreignMaster(0x00000400u));
    TEST_ASSERT_FALSE(w.foreignMaster(0x00000300u + PanelBusWatch::GONE_MS));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_panel_alone_is_no_master);
    RUN_TEST(test_a_stock_radio_is_a_master_from_its_second_sync);
    RUN_TEST(test_strays_hours_apart_are_not_a_master);
    RUN_TEST(test_a_master_quiet_for_the_gone_time_is_gone);
    RUN_TEST(test_a_lone_cluster_reads_as_cluster_only);
    RUN_TEST(test_other_3cf_frames_count_for_neither);
    RUN_TEST(test_the_windows_survive_a_millis_wrap);
    return UNITY_END();
}
