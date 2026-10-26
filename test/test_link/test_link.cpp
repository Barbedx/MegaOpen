#include <unity.h>
#include <string.h>
#include <string>
#include <vector>
#include "link/LinkFrame.h"

// The USB link's frames (docs/design/usb-link.md): what the app and the board
// both have to agree on byte for byte.

using namespace Link;

namespace
{
    struct Got { uint8_t type, flags; uint16_t chan; std::string payload; };
    std::vector<Got> g_got;
    void onFrame(const Frame& f, void*)
    {
        g_got.push_back(Got{f.type, f.flags, f.chan, std::string((const char*)f.payload, f.len)});
    }
    std::vector<uint8_t> frame(uint8_t type, uint8_t flags, uint16_t chan, const std::string& p)
    {
        std::vector<uint8_t> out(MAX_FRAME);
        out.resize(encode(type, flags, chan, (const uint8_t*)p.data(), p.size(), out.data()));
        return out;
    }
}

void setUp() { g_got.clear(); }
void tearDown() {}

void test_crc_check_value()
{
    // CRC-16/CCITT-FALSE's check value: "123456789" -> 0x29B1.
    TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16((const uint8_t*)"123456789", 9));
}

void test_encode_layout()
{
    const std::vector<uint8_t> f = frame(HTTP_REQ, FIN, 0x0102, "GET");
    TEST_ASSERT_EQUAL(HEAD + 3 + 2, f.size());
    const uint8_t head[] = {0x4D, 0x4F, 0x10, 0x01, 0x02, 0x01, 0x03, 0x00, 'G', 'E', 'T'};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, f.data(), sizeof(head));
    const uint16_t c = crc16(f.data() + 2, HEAD - 2 + 3);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)c, f[11]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(c >> 8), f[12]);
}

void test_too_long_is_refused()
{
    std::vector<uint8_t> out(MAX_FRAME + 10);
    std::vector<uint8_t> p(MAX_PAYLOAD + 1, 'x');
    TEST_ASSERT_EQUAL(0, encode(EVENT, 0, 0, p.data(), p.size(), out.data()));
    TEST_ASSERT_EQUAL(MAX_FRAME, encode(EVENT, 0, 0, p.data(), MAX_PAYLOAD, out.data()));
}

void test_round_trip_byte_by_byte()
{
    Parser p;
    std::vector<uint8_t> s = frame(HELLO, 0, 0, "{\"app\":\"1.6.0\"}");
    const std::vector<uint8_t> b = frame(PING, 0, 7, "");
    s.insert(s.end(), b.begin(), b.end());
    for (uint8_t c : s) p.feed(&c, 1, onFrame, nullptr);
    TEST_ASSERT_EQUAL(2, g_got.size());
    TEST_ASSERT_EQUAL_HEX8(HELLO, g_got[0].type);
    TEST_ASSERT_EQUAL_STRING("{\"app\":\"1.6.0\"}", g_got[0].payload.c_str());
    TEST_ASSERT_EQUAL_HEX8(PING, g_got[1].type);
    TEST_ASSERT_EQUAL(7, g_got[1].chan);
    TEST_ASSERT_EQUAL(0, g_got[1].payload.size());
    TEST_ASSERT_EQUAL(0, p.bad());
}

void test_full_payload_in_one_feed()
{
    Parser p;
    const std::string big(MAX_PAYLOAD, 'z');
    std::vector<uint8_t> s = frame(HTTP_RESP, FIN, 1, big);
    const std::vector<uint8_t> b = frame(HTTP_RESP, 0, 2, "tail");
    s.insert(s.end(), b.begin(), b.end());
    p.feed(s.data(), s.size(), onFrame, nullptr);
    TEST_ASSERT_EQUAL(2, g_got.size());
    TEST_ASSERT_EQUAL(MAX_PAYLOAD, g_got[0].payload.size());
    TEST_ASSERT_EQUAL(FIN, g_got[0].flags);
    TEST_ASSERT_EQUAL_STRING("tail", g_got[1].payload.c_str());
}

void test_garbage_and_bad_crc_resync()
{
    Parser p;
    std::vector<uint8_t> s = {0x00, 0x4D, 0x11, 0x4D};          // noise, a lone 'M'
    std::vector<uint8_t> bad = frame(EVENT, 0, 0, "lost");
    bad[HEAD] ^= 0xFF;                                         // payload hit: CRC fails
    s.insert(s.end(), bad.begin(), bad.end());
    const std::vector<uint8_t> good = frame(EVENT, 0, 0, "kept");
    s.insert(s.end(), good.begin(), good.end());
    p.feed(s.data(), s.size(), onFrame, nullptr);
    TEST_ASSERT_EQUAL(1, g_got.size());
    TEST_ASSERT_EQUAL_STRING("kept", g_got[0].payload.c_str());
    TEST_ASSERT_TRUE(p.bad() > 0);
}

void test_cut_frame_then_new_one()
{
    // A replug: half a frame, then a whole one. Only the half is lost.
    Parser p;
    const std::vector<uint8_t> a = frame(HTTP_RESP, 0, 1, std::string(300, 'a'));
    std::vector<uint8_t> s(a.begin(), a.begin() + 100);
    const std::vector<uint8_t> b = frame(PONG, 0, 0, "ok");
    s.insert(s.end(), b.begin(), b.end());
    p.feed(s.data(), s.size(), onFrame, nullptr);
    // At once: the half frame's length (300) does not hold the PONG behind it
    // until 300 bytes have come (on a quiet link, a minute of pings).
    TEST_ASSERT_EQUAL(1, g_got.size());
    TEST_ASSERT_EQUAL_HEX8(PONG, g_got[0].type);
    TEST_ASSERT_TRUE(p.bad() > 0);
    // And the stream goes on normally after it.
    const std::vector<uint8_t> c = frame(EVENT, 0, 0, "next");
    p.feed(c.data(), c.size(), onFrame, nullptr);
    TEST_ASSERT_EQUAL(2, g_got.size());
    TEST_ASSERT_EQUAL_STRING("next", g_got[1].payload.c_str());
}

void test_long_frame_in_pieces_is_not_cut()
{
    // A whole frame's own payload arriving in pieces must not be mistaken for
    // a cut one: nothing valid follows it yet.
    Parser p;
    const std::vector<uint8_t> a = frame(HTTP_RESP, 0, 1, std::string(1000, 'q'));
    for (size_t i = 0; i < a.size(); i += 64) {
        const size_t n = a.size() - i < 64 ? a.size() - i : 64;
        p.feed(a.data() + i, n, onFrame, nullptr);
    }
    TEST_ASSERT_EQUAL(1, g_got.size());
    TEST_ASSERT_EQUAL(1000, g_got[0].payload.size());
    TEST_ASSERT_EQUAL(0, p.bad());
}

void test_oversize_length_is_garbage()
{
    Parser p;
    std::vector<uint8_t> s = {0x4D, 0x4F, 0x90, 0x00, 0x01, 0x00, 0xFF, 0xFF};   // len 65535
    const std::vector<uint8_t> good = frame(PONG, 0, 0, "");
    s.insert(s.end(), good.begin(), good.end());
    p.feed(s.data(), s.size(), onFrame, nullptr);
    TEST_ASSERT_EQUAL(1, g_got.size());
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc_check_value);
    RUN_TEST(test_encode_layout);
    RUN_TEST(test_too_long_is_refused);
    RUN_TEST(test_round_trip_byte_by_byte);
    RUN_TEST(test_full_payload_in_one_feed);
    RUN_TEST(test_garbage_and_bad_crc_resync);
    RUN_TEST(test_cut_frame_then_new_one);
    RUN_TEST(test_oversize_length_is_garbage);
    RUN_TEST(test_long_frame_in_pieces_is_not_cut);
    return UNITY_END();
}
