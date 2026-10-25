#include <unity.h>
#include <string.h>
#include <vector>
#include "diag/ClimDiag.h"

// The one transmit on the car bus (src/diag/ClimDiag.h): what goes out, what
// a reply must look like, and that nothing else ever leaves.

using namespace Diag;

namespace
{
    struct Sent { uint32_t id; std::vector<uint8_t> d; };
    std::vector<Sent> g_sent;
    bool g_sendOk = true;
    bool fakeSend(uint32_t id, const uint8_t* d, uint8_t len, void*)
    {
        if (!g_sendOk) return false;
        g_sent.push_back(Sent{id, std::vector<uint8_t>(d, d + len)});
        return true;
    }
    const uint8_t REQ[] = {0x21, 0xC1};

    void rx(Exchange& e, std::initializer_list<uint8_t> b, uint32_t now, uint32_t id = CLIM_RESP)
    {
        std::vector<uint8_t> v(b);
        e.onFrame(id, v.data(), (uint8_t)v.size(), now);
    }
}

void setUp() { g_sent.clear(); g_sendOk = true; }
void tearDown() {}

void test_only_21_c1_is_allowed_and_nothing_else_leaves()
{
    Exchange e(fakeSend, nullptr);
    const uint8_t session[] = {0x10, 0xC0}, clear[] = {0x14, 0xFF, 0x00}, other[] = {0x21, 0xC2};
    TEST_ASSERT_FALSE(e.start(session, 2, 0));
    TEST_ASSERT_TRUE(e.status() == Exchange::Status::Refused);
    TEST_ASSERT_FALSE(e.start(clear, 3, 0));
    TEST_ASSERT_FALSE(e.start(other, 2, 0));
    TEST_ASSERT_FALSE(e.start(nullptr, 0, 0));
    TEST_ASSERT_EQUAL(0, (int)g_sent.size());
}

void test_the_request_is_02_21_c1_to_0x744_unpadded()
{
    Exchange e(fakeSend, nullptr);
    TEST_ASSERT_TRUE(e.start(REQ, 2, 100));
    TEST_ASSERT_EQUAL(1, (int)g_sent.size());
    TEST_ASSERT_EQUAL_HEX32(0x744, g_sent[0].id);
    const uint8_t want[] = {0x02, 0x21, 0xC1};
    TEST_ASSERT_EQUAL(3, (int)g_sent[0].d.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, g_sent[0].d.data(), 3);
    TEST_ASSERT_FALSE(e.start(REQ, 2, 101));              // one at a time
    TEST_ASSERT_EQUAL(1, (int)g_sent.size());
}

void test_a_multi_frame_reply_gets_one_flow_control_and_decodes()
{
    Exchange e(fakeSend, nullptr);
    e.start(REQ, 2, 0);
    // 61 C1, id 01, outside 49 (9 C), UCH 02, water 130 (90 C), ECM 03,
    // evap 100 (10.0 C), cabin 84 (2.0 C), sun 20 (60 W/m2), 00, batt 176.
    rx(e, {0x10, 0x0C, 0x61, 0xC1, 0x01, 49, 0x02, 130}, 10);
    TEST_ASSERT_EQUAL(2, (int)g_sent.size());
    const uint8_t fc[] = {0x30, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX32(0x744, g_sent[1].id);
    TEST_ASSERT_EQUAL(3, (int)g_sent[1].d.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(fc, g_sent[1].d.data(), 3);
    TEST_ASSERT_TRUE(e.status() == Exchange::Status::Waiting);
    rx(e, {0x21, 0x03, 100, 84, 20, 0x00, 176, 0xAA}, 20);   // past the length: ignored
    TEST_ASSERT_TRUE(e.status() == Exchange::Status::Ok);
    TEST_ASSERT_EQUAL(12, e.replyLen());
    TEST_ASSERT_EQUAL(2, (int)g_sent.size());               // no second flow control
    ClimValues v;
    TEST_ASSERT_TRUE(decode21C1(e.reply(), e.replyLen(), v));
    TEST_ASSERT_EQUAL_INT16(20, v.cabinX10);
    TEST_ASSERT_EQUAL_INT16(100, v.evapX10);
    TEST_ASSERT_EQUAL_INT16(9, v.outsideC);
    TEST_ASSERT_EQUAL_INT16(90, v.coolantC);
    TEST_ASSERT_EQUAL_UINT16(60, v.sunWm2);
    TEST_ASSERT_EQUAL_UINT16(1373, v.battX100);              // 176 x 0.078 = 13.73 V
}

void test_other_ids_and_frames_before_the_request_are_ignored()
{
    Exchange e(fakeSend, nullptr);
    rx(e, {0x03, 0x61, 0xC1, 0x00}, 0);                      // nothing asked
    TEST_ASSERT_TRUE(e.status() == Exchange::Status::Idle);
    e.start(REQ, 2, 0);
    rx(e, {0x10, 0x0C, 0x61, 0xC1, 1, 2, 3, 4}, 5, 0x763);   // another ECU
    TEST_ASSERT_EQUAL(1, (int)g_sent.size());                // no flow control for it
    TEST_ASSERT_TRUE(e.status() == Exchange::Status::Waiting);
}

void test_a_negative_reply_and_response_pending()
{
    Exchange e(fakeSend, nullptr);
    e.start(REQ, 2, 0);
    rx(e, {0x03, 0x7F, 0x21, 0x78}, 500);                    // pending: wait on, longer
    TEST_ASSERT_TRUE(e.status() == Exchange::Status::Waiting);
    e.tick(500 + REPLY_TIMEOUT_MS + 100);
    TEST_ASSERT_TRUE(e.status() == Exchange::Status::Waiting);
    rx(e, {0x03, 0x7F, 0x21, 0x12}, 2000);
    TEST_ASSERT_TRUE(e.status() == Exchange::Status::Negative);
    TEST_ASSERT_EQUAL_HEX8(0x12, e.nrc());
}

void test_timeout_bad_sequence_and_a_refusing_controller()
{
    Exchange e(fakeSend, nullptr);
    e.start(REQ, 2, 0);
    e.tick(REPLY_TIMEOUT_MS - 1);
    TEST_ASSERT_TRUE(e.status() == Exchange::Status::Waiting);
    e.tick(REPLY_TIMEOUT_MS);
    TEST_ASSERT_TRUE(e.status() == Exchange::Status::Timeout);

    e.clear();
    e.start(REQ, 2, 0);
    rx(e, {0x10, 0x0C, 0x61, 0xC1, 1, 2, 3, 4}, 1);
    rx(e, {0x22, 5, 6, 7, 8, 9, 10, 11}, 2);                 // seq 2, wanted 1
    TEST_ASSERT_TRUE(e.status() == Exchange::Status::Bad);

    g_sendOk = false;
    e.clear();
    TEST_ASSERT_FALSE(e.start(REQ, 2, 0));
    TEST_ASSERT_TRUE(e.status() == Exchange::Status::SendFailed);
}

void test_decode_wants_61_c1()
{
    ClimValues v;
    const uint8_t wrong[] = {0x61, 0xC2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t shortr[] = {0x61, 0xC1, 0, 0};
    TEST_ASSERT_FALSE(decode21C1(wrong, sizeof(wrong), v));
    TEST_ASSERT_FALSE(decode21C1(shortr, sizeof(shortr), v));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_only_21_c1_is_allowed_and_nothing_else_leaves);
    RUN_TEST(test_the_request_is_02_21_c1_to_0x744_unpadded);
    RUN_TEST(test_a_multi_frame_reply_gets_one_flow_control_and_decodes);
    RUN_TEST(test_other_ids_and_frames_before_the_request_are_ignored);
    RUN_TEST(test_a_negative_reply_and_response_pending);
    RUN_TEST(test_timeout_bad_sequence_and_a_refusing_controller);
    RUN_TEST(test_decode_wants_61_c1);
    return UNITY_END();
}
