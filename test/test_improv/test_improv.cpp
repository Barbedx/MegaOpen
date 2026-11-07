#include <unity.h>
#include <string.h>
#include <string>
#include <vector>
#include "net/Improv.h"

// Improv Wi-Fi over serial (src/net/Improv.h): what the web installer's
// «Configure Wi-Fi» sends and what it must get back. The packet bytes are the
// spec's (improv-wifi.com/serial), so a wrong byte here is a dialog that hangs.

void setUp() {}
void tearDown() {}

using namespace Improv;

struct Pkt { uint8_t type; std::vector<uint8_t> data; };

struct FakePort : Port {
    std::vector<uint8_t> out;
    void write(const uint8_t* d, size_t n) override { out.insert(out.end(), d, d + n); }
    // Splits what was written into packets, checking each frame.
    std::vector<Pkt> packets()
    {
        std::vector<Pkt> r;
        size_t i = 0;
        while (i < out.size()) {
            TEST_ASSERT_TRUE(i + 11 <= out.size());
            TEST_ASSERT_EQUAL_MEMORY("IMPROV", &out[i], 6);
            TEST_ASSERT_EQUAL_UINT8(1, out[i + 6]);
            const uint8_t len = out[i + 8];
            uint8_t sum = 0;
            for (size_t k = 0; k < 9u + len; ++k) sum += out[i + k];
            TEST_ASSERT_EQUAL_UINT8(sum, out[i + 9 + len]);
            TEST_ASSERT_EQUAL_UINT8('\n', out[i + 10 + len]);
            r.push_back({out[i + 7], std::vector<uint8_t>(out.begin() + i + 9, out.begin() + i + 9 + len)});
            i += 11 + len;
        }
        out.clear();
        return r;
    }
};

struct FakeBoard : Board {
    bool up = false;
    Join j = Join::None;
    std::string ssid, pass;
    int provisions = 0;
    bool scanOk = true, scanRunning = false;
    std::vector<Net> nets;
    bool connected() override { return up; }
    void url(char* o, size_t c) override { snprintf(o, c, "http://192.168.1.50"); }
    const char* firmware() override { return "MegaOpen"; }
    const char* version() override { return "1.10.0"; }
    const char* chip() override { return "ESP32-S3"; }
    const char* deviceName() override { return "MegaOpen-8428"; }
    void provision(const char* s, const char* p) override { ssid = s; pass = p; ++provisions; j = Join::Pending; }
    Join join() override { return j; }
    bool scanStart() override { scanRunning = scanOk; return scanOk; }
    int scanDone(Net* o, int cap) override
    {
        if (scanRunning) return -1;
        int k = 0;
        for (auto& n : nets) if (k < cap) o[k++] = n;
        return k;
    }
};

// One RPC from the client, framed as the client frames it.
static std::vector<uint8_t> rpc(uint8_t cmd, const std::vector<uint8_t>& args)
{
    std::vector<uint8_t> d{cmd, (uint8_t)args.size()};
    d.insert(d.end(), args.begin(), args.end());
    uint8_t b[300];
    const size_t n = frame(Rpc, d.data(), d.size(), b, sizeof(b));
    // The client sends no '\n'; drop ours.
    return std::vector<uint8_t>(b, b + n - 1);
}

static void feed(Session& s, const std::vector<uint8_t>& v, uint32_t t = 0)
{
    for (uint8_t c : v) s.feed(c, t);
}

// The strings of an RPC result: [cmd, len, (l, bytes)...].
static std::vector<std::string> strings(const Pkt& p)
{
    std::vector<std::string> r;
    TEST_ASSERT_EQUAL_UINT8(RpcResult, p.type);
    TEST_ASSERT_EQUAL_UINT8(p.data.size() - 2, p.data[1]);
    size_t i = 2;
    while (i < p.data.size()) {
        const uint8_t l = p.data[i++];
        r.emplace_back((const char*)&p.data[i], l);
        i += l;
    }
    return r;
}

static std::vector<uint8_t> wifiArgs(const char* s, const char* p)
{
    std::vector<uint8_t> a{(uint8_t)strlen(s)};
    a.insert(a.end(), s, s + strlen(s));
    a.push_back((uint8_t)strlen(p));
    a.insert(a.end(), p, p + strlen(p));
    return a;
}

void test_frame_matches_the_spec_bytes()
{
    // Current state "authorized": IMPROV 01 01 01 02, checksum, '\n'.
    uint8_t b[16];
    const uint8_t d = 0x02;
    TEST_ASSERT_EQUAL(12, frame(CurrentState, &d, 1, b, sizeof(b)));
    const uint8_t want[] = {'I', 'M', 'P', 'R', 'O', 'V', 1, 1, 1, 2, 0, '\n'};
    uint8_t sum = 0;
    for (int i = 0; i < 10; ++i) sum += want[i];
    TEST_ASSERT_EQUAL_MEMORY(want, b, 10);
    TEST_ASSERT_EQUAL_UINT8(sum, b[10]);
    TEST_ASSERT_EQUAL_UINT8('\n', b[11]);
    TEST_ASSERT_EQUAL(0, frame(CurrentState, &d, 1, b, 11));   // too small
}

void test_state_unprovisioned_is_authorized()
{
    FakeBoard b; FakePort p; Session s(b, p);
    feed(s, rpc(GetState, {}));
    auto r = p.packets();
    TEST_ASSERT_EQUAL(1, r.size());
    TEST_ASSERT_EQUAL_UINT8(CurrentState, r[0].type);
    TEST_ASSERT_EQUAL_UINT8(Authorized, r[0].data[0]);
}

void test_state_on_a_network_is_provisioned_with_the_url()
{
    FakeBoard b; b.up = true; FakePort p; Session s(b, p);
    feed(s, rpc(GetState, {}));
    auto r = p.packets();
    TEST_ASSERT_EQUAL(2, r.size());
    TEST_ASSERT_EQUAL_UINT8(Provisioned, r[0].data[0]);
    auto str = strings(r[1]);
    TEST_ASSERT_EQUAL_UINT8(GetState, r[1].data[0]);
    TEST_ASSERT_EQUAL(1, str.size());
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.50", str[0].c_str());
}

void test_device_info_is_four_strings()
{
    FakeBoard b; FakePort p; Session s(b, p);
    feed(s, rpc(GetInfo, {}));
    auto r = p.packets();
    TEST_ASSERT_EQUAL(1, r.size());
    auto str = strings(r[0]);
    TEST_ASSERT_EQUAL(4, str.size());
    TEST_ASSERT_EQUAL_STRING("MegaOpen", str[0].c_str());
    TEST_ASSERT_EQUAL_STRING("1.10.0", str[1].c_str());
    TEST_ASSERT_EQUAL_STRING("ESP32-S3", str[2].c_str());
    TEST_ASSERT_EQUAL_STRING("MegaOpen-8428", str[3].c_str());
}

void test_scan_lists_each_network_then_an_empty_end()
{
    FakeBoard b; FakePort p; Session s(b, p);
    b.nets = {{"Yozhik", -48, true}, {"cafe", -80, false}};
    b.scanRunning = true;
    feed(s, rpc(GetNetworks, {}));
    s.tick(10);
    TEST_ASSERT_EQUAL(0, p.packets().size());     // still scanning: nothing yet
    b.scanRunning = false;
    s.tick(20);
    auto r = p.packets();
    TEST_ASSERT_EQUAL(3, r.size());
    auto a = strings(r[0]), c = strings(r[1]), e = strings(r[2]);
    TEST_ASSERT_EQUAL_STRING("Yozhik", a[0].c_str());
    TEST_ASSERT_EQUAL_STRING("-48", a[1].c_str());
    TEST_ASSERT_EQUAL_STRING("YES", a[2].c_str());
    TEST_ASSERT_EQUAL_STRING("NO", c[2].c_str());
    TEST_ASSERT_EQUAL(0, e.size());
    TEST_ASSERT_EQUAL_UINT8(GetNetworks, r[2].data[0]);
}

void test_a_scan_that_cannot_start_answers_the_empty_end_at_once()
{
    FakeBoard b; b.scanOk = false; FakePort p; Session s(b, p);
    feed(s, rpc(GetNetworks, {}));
    auto r = p.packets();
    TEST_ASSERT_EQUAL(1, r.size());
    TEST_ASSERT_EQUAL(0, strings(r[0]).size());
}

void test_wifi_settings_join_then_provisioned_with_the_url()
{
    FakeBoard b; FakePort p; Session s(b, p);
    feed(s, rpc(WifiSettings, wifiArgs("Yozhik", "secret123")), 1000);
    TEST_ASSERT_EQUAL(1, b.provisions);
    TEST_ASSERT_EQUAL_STRING("Yozhik", b.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("secret123", b.pass.c_str());
    auto r = p.packets();
    TEST_ASSERT_EQUAL_UINT8(ErrorState, r[0].type);
    TEST_ASSERT_EQUAL_UINT8(NoError, r[0].data[0]);
    TEST_ASSERT_EQUAL_UINT8(Provisioning, r.back().data[0]);
    s.tick(2000);
    TEST_ASSERT_EQUAL(0, p.packets().size());     // still joining
    b.j = Board::Join::Joined;
    b.up = true;
    s.tick(3000);
    r = p.packets();
    TEST_ASSERT_EQUAL(2, r.size());
    TEST_ASSERT_EQUAL_UINT8(Provisioned, r[0].data[0]);
    TEST_ASSERT_EQUAL_UINT8(WifiSettings, r[1].data[0]);
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.50", strings(r[1])[0].c_str());
    TEST_ASSERT_EQUAL(Provisioned, s.state());
}

void test_a_failed_join_is_unable_to_connect_and_back_to_authorized()
{
    FakeBoard b; FakePort p; Session s(b, p);
    feed(s, rpc(WifiSettings, wifiArgs("Yozhik", "wrongpass")), 0);
    p.packets();
    b.j = Board::Join::Failed;
    s.tick(100);
    auto r = p.packets();
    TEST_ASSERT_EQUAL(2, r.size());
    TEST_ASSERT_EQUAL_UINT8(ErrorState, r[0].type);
    TEST_ASSERT_EQUAL_UINT8(UnableToConnect, r[0].data[0]);
    TEST_ASSERT_EQUAL_UINT8(Authorized, r[1].data[0]);
}

void test_a_join_that_never_ends_times_out()
{
    FakeBoard b; FakePort p; Session s(b, p);
    feed(s, rpc(WifiSettings, wifiArgs("Yozhik", "secret123")), 0);
    p.packets();
    s.tick(JOIN_TIMEOUT_MS - 1);
    TEST_ASSERT_EQUAL(0, p.packets().size());
    s.tick(JOIN_TIMEOUT_MS);
    auto r = p.packets();
    TEST_ASSERT_EQUAL_UINT8(UnableToConnect, r[0].data[0]);
}

void test_state_while_joining_is_provisioning()
{
    FakeBoard b; FakePort p; Session s(b, p);
    feed(s, rpc(WifiSettings, wifiArgs("Yozhik", "secret123")));
    p.packets();
    feed(s, rpc(GetState, {}));
    auto r = p.packets();
    TEST_ASSERT_EQUAL(1, r.size());
    TEST_ASSERT_EQUAL_UINT8(Provisioning, r[0].data[0]);
}

void test_an_open_network_has_an_empty_password()
{
    FakeBoard b; FakePort p; Session s(b, p);
    feed(s, rpc(WifiSettings, wifiArgs("cafe", "")));
    TEST_ASSERT_EQUAL(1, b.provisions);
    TEST_ASSERT_EQUAL_STRING("", b.pass.c_str());
}

void test_bad_wifi_args_are_invalid_and_join_nothing()
{
    FakeBoard b; FakePort p; Session s(b, p);
    std::vector<uint8_t> a = wifiArgs("Yozhik", "secret123");
    a.pop_back();                                  // pass shorter than its length
    feed(s, rpc(WifiSettings, a));
    feed(s, rpc(WifiSettings, wifiArgs("", "secret123")));   // no name
    TEST_ASSERT_EQUAL(0, b.provisions);
    auto r = p.packets();
    TEST_ASSERT_EQUAL(2, r.size());
    TEST_ASSERT_EQUAL_UINT8(InvalidRpc, r[0].data[0]);
    TEST_ASSERT_EQUAL_UINT8(InvalidRpc, r[1].data[0]);
}

void test_a_bad_checksum_is_an_invalid_rpc()
{
    FakeBoard b; FakePort p; Session s(b, p);
    auto v = rpc(GetState, {});
    v.back() ^= 0xFF;
    feed(s, v);
    auto r = p.packets();
    TEST_ASSERT_EQUAL(1, r.size());
    TEST_ASSERT_EQUAL_UINT8(ErrorState, r[0].type);
    TEST_ASSERT_EQUAL_UINT8(InvalidRpc, r[0].data[0]);
    TEST_ASSERT_EQUAL(1, s.bad());
}

void test_an_unknown_command_is_unknown_rpc()
{
    FakeBoard b; FakePort p; Session s(b, p);
    feed(s, rpc(0x05, {}));   // hostname: not offered
    auto r = p.packets();
    TEST_ASSERT_EQUAL_UINT8(UnknownRpc, r[0].data[0]);
}

void test_noise_around_a_packet_is_skipped()
{
    // Log text and a false start ("IMPRI") before the real packet.
    FakeBoard b; FakePort p; Session s(b, p);
    const char* junk = "I [wifi] joined\nIMPRIIMP";
    for (const char* c = junk; *c; ++c) s.feed((uint8_t)*c, 0);
    feed(s, rpc(GetState, {}));
    auto r = p.packets();
    TEST_ASSERT_EQUAL(1, r.size());
    TEST_ASSERT_EQUAL_UINT8(Authorized, r[0].data[0]);
    TEST_ASSERT_EQUAL(1, s.packets());
    TEST_ASSERT_EQUAL(0, s.bad());
}

void test_a_packet_split_across_feeds_still_parses()
{
    FakeBoard b; FakePort p; Session s(b, p);
    auto v = rpc(GetInfo, {});
    for (size_t i = 0; i < v.size(); ++i) { s.feed(v[i], 0); s.tick(0); }
    TEST_ASSERT_EQUAL(1, p.packets().size());
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_frame_matches_the_spec_bytes);
    RUN_TEST(test_state_unprovisioned_is_authorized);
    RUN_TEST(test_state_on_a_network_is_provisioned_with_the_url);
    RUN_TEST(test_device_info_is_four_strings);
    RUN_TEST(test_scan_lists_each_network_then_an_empty_end);
    RUN_TEST(test_a_scan_that_cannot_start_answers_the_empty_end_at_once);
    RUN_TEST(test_wifi_settings_join_then_provisioned_with_the_url);
    RUN_TEST(test_a_failed_join_is_unable_to_connect_and_back_to_authorized);
    RUN_TEST(test_a_join_that_never_ends_times_out);
    RUN_TEST(test_state_while_joining_is_provisioning);
    RUN_TEST(test_an_open_network_has_an_empty_password);
    RUN_TEST(test_bad_wifi_args_are_invalid_and_join_nothing);
    RUN_TEST(test_a_bad_checksum_is_an_invalid_rpc);
    RUN_TEST(test_an_unknown_command_is_unknown_rpc);
    RUN_TEST(test_noise_around_a_packet_is_skipped);
    RUN_TEST(test_a_packet_split_across_feeds_still_parses);
    return UNITY_END();
}
