// Who else is on the panel bus (display/PanelBusWatch.h), on timed frame
// replays. The failure it prevents is silent on hardware: two masters fighting
// over one panel look like a panel that "sometimes" registers, so when the TX
// gate may open, and what silences it, are pinned here.
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

// Our bench panel alone: the gate opens LISTEN_MS after the FIRST POLL
// (begin()), not after setup.
void test_a_panel_alone_opens_the_gate_after_the_listen_window()
{
    PanelBusWatch w;
    TEST_ASSERT_FALSE(w.mayTransmit(60000));               // before the first poll: never
    w.begin(5000);
    w.begin(9000);                                         // later calls change nothing
    for (uint32_t t = 5000; t < 9000; t += 100) panelFrames(w, t);
    TEST_ASSERT_FALSE(w.mayTransmit(5000 + PanelBusWatch::LISTEN_MS - 1));
    TEST_ASSERT_TRUE(w.mayTransmit(5000 + PanelBusWatch::LISTEN_MS));
    TEST_ASSERT_FALSE(w.foreignMaster(9000));
    TEST_ASSERT_FALSE(w.clusterOnly());
}

// The review's case: an OEM radio syncing at ~1 s. Syncs at 900 and 1900 ms:
// the first already keeps the gate shut, the second makes it a master; the
// gate never opens while it keeps syncing.
void test_an_oem_radio_never_lets_the_gate_open()
{
    PanelBusWatch w;
    w.begin(0);
    w.onRx(0x3AF, 2, kSync, 900);
    TEST_ASSERT_FALSE(w.foreignMaster(900));               // one sync is no master yet...
    TEST_ASSERT_FALSE(w.mayTransmit(3000));                // ...but it keeps us quiet
    w.onRx(0x3AF, 2, kSync, 1900);
    TEST_ASSERT_TRUE(w.foreignMaster(1900));
    TEST_ASSERT_EQUAL_HEX32(0x3AF, w.masterId());
    for (uint32_t t = 2900; t < 60000; t += 1000) {
        w.onRx(0x3AF, 2, kSync, t);
        TEST_ASSERT_FALSE(w.mayTransmit(t + 500));
        TEST_ASSERT_TRUE(w.foreignMaster(t + 500));
    }
}

// A sync that arrives after the gate opened shuts it again at once (before a
// second one would make it a master).
void test_a_late_sync_shuts_an_open_gate()
{
    PanelBusWatch w;
    w.begin(0);
    TEST_ASSERT_TRUE(w.mayTransmit(PanelBusWatch::LISTEN_MS));
    w.onRx(0x3DF, 2, kSync, 10000);
    TEST_ASSERT_FALSE(w.mayTransmit(10001));
    TEST_ASSERT_FALSE(w.foreignMaster(10001));
    TEST_ASSERT_EQUAL_HEX32(0x3DF, w.masterId());
    // One stray, nothing after it: the gate opens again LISTEN_MS later.
    TEST_ASSERT_TRUE(w.mayTransmit(10000 + PanelBusWatch::LISTEN_MS));
}

// Two strays hours apart are not a master: the count is within WINDOW_MS.
void test_strays_hours_apart_are_not_a_master()
{
    PanelBusWatch w;
    w.begin(0);
    w.onRx(0x3AF, 2, kSync, 100000);
    w.onRx(0x3AF, 2, kSync, 100000 + 3600000);
    TEST_ASSERT_FALSE(w.foreignMaster(100000 + 3600000));
    TEST_ASSERT_TRUE(w.mayTransmit(100000 + 3600000 + PanelBusWatch::LISTEN_MS));
}

// A master that goes quiet for REARM_MS is gone; a single sync after that
// starts a fresh count instead of re-latching at once.
void test_a_master_quiet_for_the_rearm_time_is_gone()
{
    PanelBusWatch w;
    w.begin(0);
    w.onRx(0x3AF, 2, kSync, 1000);
    w.onRx(0x3AF, 2, kSync, 2000);
    TEST_ASSERT_TRUE(w.foreignMaster(2000 + PanelBusWatch::REARM_MS - 1));
    TEST_ASSERT_FALSE(w.foreignMaster(2000 + PanelBusWatch::REARM_MS));
    TEST_ASSERT_TRUE(w.mayTransmit(2000 + PanelBusWatch::REARM_MS));
    const uint32_t later = 2000 + PanelBusWatch::REARM_MS + 5000;
    w.onRx(0x3AF, 2, kSync, later);
    TEST_ASSERT_FALSE(w.foreignMaster(later));             // one sync: not re-latched
    TEST_ASSERT_FALSE(w.mayTransmit(later + 1));           // but quiet again
    w.onRx(0x3AF, 2, kSync, later + 900);
    TEST_ASSERT_TRUE(w.foreignMaster(later + 900));        // a second within the window: master
}

// A lone instrument cluster: 0x3CF DLC 1 0x69 pings, never the panel's 61 11.
void test_a_lone_cluster_reads_as_cluster_only()
{
    PanelBusWatch w;
    w.begin(0);
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
    w.begin(0);
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
    w.begin(0xFFFFFF00u);
    TEST_ASSERT_FALSE(w.mayTransmit(0xFFFFFF00u + 100));
    TEST_ASSERT_TRUE(w.mayTransmit(0xFFFFFF00u + PanelBusWatch::LISTEN_MS));
    w.onRx(0x3AF, 2, kSync, 0xFFFFFFF0u);
    w.onRx(0x3AF, 2, kSync, 0x00000300u);
    TEST_ASSERT_TRUE(w.foreignMaster(0x00000400u));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_panel_alone_opens_the_gate_after_the_listen_window);
    RUN_TEST(test_an_oem_radio_never_lets_the_gate_open);
    RUN_TEST(test_a_late_sync_shuts_an_open_gate);
    RUN_TEST(test_strays_hours_apart_are_not_a_master);
    RUN_TEST(test_a_master_quiet_for_the_rearm_time_is_gone);
    RUN_TEST(test_a_lone_cluster_reads_as_cluster_only);
    RUN_TEST(test_other_3cf_frames_count_for_neither);
    RUN_TEST(test_the_windows_survive_a_millis_wrap);
    return UNITY_END();
}
