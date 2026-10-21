// Who else is on the panel bus (display/PanelBusWatch.h), on frame replays.
// The failure it prevents is silent on hardware: two masters fighting over
// one panel look like a panel that "sometimes" registers, so the verdicts and
// the TX gate's opening are pinned here.
#include <unity.h>
#include <stdint.h>
#include "display/PanelBusWatch.h"

using MegaOpen::PanelBusWatch;

void setUp() {}
void tearDown() {}

struct Rx { uint32_t id; uint8_t len; uint8_t d[8]; };

static void replay(PanelBusWatch& w, const Rx* f, int n, int times = 1)
{
    for (int t = 0; t < times; ++t)
        for (int i = 0; i < n; ++i) w.onRx(f[i].id, f[i].len, f[i].d);
}

// What our bench panel sends alone, before and after registration: its
// 0x3CF request, its 1C1 70 opening, a key frame. No master but us.
static const Rx kPanelAlone[] = {
    {0x3CF, 3, {0x61, 0x11, 0x01}},
    {0x3CF, 3, {0x61, 0x11, 0x00}},
    {0x1C1, 8, {0x70, 0x00}},
    {0x1C1, 8, {0x03, 0x89, 0x00, 0x00}},
};

// The same panel with an OEM Carminat radio driving it: its sync on 0x3AF.
static const Rx kOemCarminat[] = {
    {0x3AF, 2, {0x79, 0x00}},
    {0x3CF, 3, {0x61, 0x11, 0x01}},
    {0x3AF, 2, {0x79, 0x00}},
    {0x1C1, 8, {0x70, 0x00}},
};

void test_a_panel_alone_lets_us_talk_after_the_listen_window()
{
    PanelBusWatch w;
    TEST_ASSERT_FALSE(w.mayTransmit(5000));            // before begin(): never
    w.begin(1000);
    replay(w, kPanelAlone, 4, 10);
    TEST_ASSERT_FALSE(w.foreignMaster());
    TEST_ASSERT_FALSE(w.clusterOnly());
    TEST_ASSERT_FALSE(w.mayTransmit(1000 + PanelBusWatch::LISTEN_MS - 1));
    TEST_ASSERT_TRUE(w.mayTransmit(1000 + PanelBusWatch::LISTEN_MS));
    TEST_ASSERT_EQUAL_UINT32(20, w.panelRequests());
}

void test_an_oem_carminat_radio_keeps_us_silent_for_good()
{
    PanelBusWatch w;
    w.begin(0);
    replay(w, kOemCarminat, 4, 3);
    TEST_ASSERT_TRUE(w.foreignMaster());
    TEST_ASSERT_EQUAL_HEX32(0x3AF, w.masterId());
    TEST_ASSERT_FALSE(w.mayTransmit(PanelBusWatch::LISTEN_MS));
    TEST_ASSERT_FALSE(w.mayTransmit(600000));
    // The panel answers it too: not a cluster.
    TEST_ASSERT_FALSE(w.clusterOnly());
}

void test_an_updatelist_radio_is_a_foreign_master_too()
{
    PanelBusWatch w;
    w.begin(0);
    const Rx f[] = {{0x3DF, 2, {0x7A, 0x01}}, {0x3CF, 2, {0x61, 0x11}}, {0x3DF, 2, {0x7A, 0x01}}};
    replay(w, f, 3);
    TEST_ASSERT_TRUE(w.foreignMaster());
    TEST_ASSERT_EQUAL_HEX32(0x3DF, w.masterId());
}

// One stray 0x3AF (a glitch, a replayed capture) does not silence us.
void test_one_stray_sync_is_not_a_master()
{
    PanelBusWatch w;
    w.begin(0);
    const Rx f[] = {{0x3AF, 2, {0x79, 0x00}}};
    replay(w, f, 1);
    replay(w, kPanelAlone, 4, 5);
    TEST_ASSERT_FALSE(w.foreignMaster());
    TEST_ASSERT_TRUE(w.mayTransmit(PanelBusWatch::LISTEN_MS));
}

// A lone instrument cluster: 0x3CF DLC 1 0x69 pings, never the panel's 61 11.
// One panel request is enough to say a panel is there after all.
void test_a_lone_cluster_reads_as_cluster_only()
{
    PanelBusWatch w;
    w.begin(0);
    const Rx ping[] = {{0x3CF, 1, {0x69}}};
    replay(w, ping, 1, 2);
    TEST_ASSERT_FALSE(w.clusterOnly());                // under CLUSTER_MIN
    replay(w, ping, 1, 3);
    TEST_ASSERT_TRUE(w.clusterOnly());
    TEST_ASSERT_FALSE(w.foreignMaster());              // a cluster is no master
    TEST_ASSERT_TRUE(w.mayTransmit(PanelBusWatch::LISTEN_MS));
    replay(w, kPanelAlone, 1);
    TEST_ASSERT_FALSE(w.clusterOnly());
}

// 0x3CF with DLC 2+ and other bytes, or DLC 1 not 0x69, counts for neither.
void test_other_3cf_frames_count_for_neither()
{
    PanelBusWatch w;
    w.begin(0);
    const Rx f[] = {{0x3CF, 1, {0x70}}, {0x3CF, 2, {0x69, 0x00}}, {0x3CF, 0, {0}}};
    replay(w, f, 3, 4);
    TEST_ASSERT_EQUAL_UINT32(0, w.clusterPings());
    TEST_ASSERT_EQUAL_UINT32(0, w.panelRequests());
}

// The listen window across millis() wrapping.
void test_the_listen_window_survives_a_millis_wrap()
{
    PanelBusWatch w;
    w.begin(0xFFFFFF00u);
    TEST_ASSERT_FALSE(w.mayTransmit(0xFFFFFF00u + 100));
    TEST_ASSERT_TRUE(w.mayTransmit(0xFFFFFF00u + PanelBusWatch::LISTEN_MS));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_panel_alone_lets_us_talk_after_the_listen_window);
    RUN_TEST(test_an_oem_carminat_radio_keeps_us_silent_for_good);
    RUN_TEST(test_an_updatelist_radio_is_a_foreign_master_too);
    RUN_TEST(test_one_stray_sync_is_not_a_master);
    RUN_TEST(test_a_lone_cluster_reads_as_cluster_only);
    RUN_TEST(test_other_3cf_frames_count_for_neither);
    RUN_TEST(test_the_listen_window_survives_a_millis_wrap);
    return UNITY_END();
}
