#include <unity.h>
#include <string.h>
#include "net/GhRelease.h"

// The GitHub update's decisions (src/net/GhRelease.h): which version is newer,
// which asset is this board's, and which hash checks it. A wrong answer either
// offers an older image as an update or flashes another board's image, so the
// edges are pinned here.

void setUp() {}
void tearDown() {}

using namespace GhRelease;

static Ver V(const char* s)
{
    Ver v;
    TEST_ASSERT_TRUE_MESSAGE(parse(s, v), s);
    return v;
}

void test_parse_accepts_the_tag_and_the_bare_version()
{
    Ver v = V("v1.2.3");
    TEST_ASSERT_EQUAL(1, v.maj); TEST_ASSERT_EQUAL(2, v.min); TEST_ASSERT_EQUAL(3, v.pat);
    TEST_ASSERT_EQUAL(-1, v.rc);
    v = V("10.0.12-rc4");
    TEST_ASSERT_EQUAL(10, v.maj); TEST_ASSERT_EQUAL(12, v.pat); TEST_ASSERT_EQUAL(4, v.rc);
}

void test_parse_rejects_what_is_not_a_version()
{
    const char* bad[] = {"", "v", "1", "1.2", "1.2.", "1.2.3.4", "1.2.3-beta1", "1.2.3-rc",
                         "1.2.3 ", "x1.2.3", "1.-2.3", "70000.0.0", "latest"};
    for (const char* s : bad) {
        Ver v;
        TEST_ASSERT_FALSE_MESSAGE(parse(s, v), s);
    }
    Ver v;
    TEST_ASSERT_FALSE(parse(nullptr, v));
}

void test_compare_orders_numbers_not_text()
{
    TEST_ASSERT_TRUE(compare(V("1.0.10"), V("1.0.9")) > 0);
    TEST_ASSERT_TRUE(compare(V("1.10.0"), V("1.9.99")) > 0);
    TEST_ASSERT_TRUE(compare(V("2.0.0"), V("1.99.99")) > 0);
    TEST_ASSERT_EQUAL(0, compare(V("v1.0.0"), V("1.0.0")));
}

void test_an_rc_is_below_its_final_and_above_the_previous()
{
    TEST_ASSERT_TRUE(compare(V("1.0.1-rc1"), V("1.0.1")) < 0);
    TEST_ASSERT_TRUE(compare(V("1.0.1"), V("1.0.1-rc9")) > 0);
    TEST_ASSERT_TRUE(compare(V("1.0.1-rc2"), V("1.0.1-rc10")) < 0);
    TEST_ASSERT_TRUE(compare(V("1.0.1-rc1"), V("1.0.0")) > 0);
    TEST_ASSERT_EQUAL(0, compare(V("1.0.1-rc3"), V("1.0.1-rc3")));
}

// A trimmed /releases/latest as release.yml publishes it, run through the filter.
static const char* REL =
    "{\"url\":\"x\",\"tag_name\":\"v1.0.1\",\"name\":\"MegaOpen 1.0.1\",\"draft\":false,"
    "\"prerelease\":false,\"body\":\"## notes\\nlong text\",\"assets\":["
    "{\"name\":\"megaopen-t2can-1.0.1-factory.bin\",\"size\":2000000,\"browser_download_url\":"
    "\"https://github.com/Barbedx/MegaOpen/releases/download/v1.0.1/megaopen-t2can-1.0.1-factory.bin\"},"
    "{\"name\":\"megaopen-t2can_usbhid-1.0.1.bin\",\"size\":1400001,\"browser_download_url\":"
    "\"https://github.com/Barbedx/MegaOpen/releases/download/v1.0.1/megaopen-t2can_usbhid-1.0.1.bin\"},"
    "{\"name\":\"megaopen-t2can-1.0.1.bin\",\"size\":1386845,\"uploader\":{\"login\":\"b\"},"
    "\"browser_download_url\":"
    "\"https://github.com/Barbedx/MegaOpen/releases/download/v1.0.1/megaopen-t2can-1.0.1.bin\"},"
    "{\"name\":\"SHA256SUMS\",\"size\":600,\"browser_download_url\":"
    "\"https://github.com/Barbedx/MegaOpen/releases/download/v1.0.1/SHA256SUMS\"}]}";

static Pick pickFrom(const char* json, const char* env, Release& r, JsonDocument& doc)
{
    JsonDocument f;
    filter(f);
    TEST_ASSERT_TRUE(deserializeJson(doc, json, DeserializationOption::Filter(f)) ==
                     DeserializationError::Ok);
    return pick(doc.as<JsonVariantConst>(), env, r);
}

void test_the_filter_keeps_only_what_pick_reads()
{
    JsonDocument doc;
    Release r;
    pickFrom(REL, "t2can", r, doc);
    TEST_ASSERT_FALSE(doc["body"].is<const char*>());
    TEST_ASSERT_FALSE(doc["assets"][2]["uploader"].is<JsonObject>());
    TEST_ASSERT_EQUAL(4, doc["assets"].size());
}

void test_pick_takes_this_envs_app_image_not_factory_not_another_env()
{
    JsonDocument doc;
    Release r;
    TEST_ASSERT_TRUE(pickFrom(REL, "t2can", r, doc) == Pick::OK);
    TEST_ASSERT_EQUAL_STRING("v1.0.1", r.tag);
    TEST_ASSERT_EQUAL_STRING("megaopen-t2can-1.0.1.bin", r.asset);
    TEST_ASSERT_EQUAL_UINT32(1386845, r.size);
    TEST_ASSERT_EQUAL_STRING(
        "https://github.com/Barbedx/MegaOpen/releases/download/v1.0.1/megaopen-t2can-1.0.1.bin", r.binUrl);
    TEST_ASSERT_EQUAL_STRING("https://github.com/Barbedx/MegaOpen/releases/download/v1.0.1/SHA256SUMS",
                             r.sumsUrl);

    TEST_ASSERT_TRUE(pickFrom(REL, "t2can_usbhid", r, doc) == Pick::OK);
    TEST_ASSERT_EQUAL_UINT32(1400001, r.size);
    TEST_ASSERT_TRUE(pickFrom(REL, "direct", r, doc) == Pick::NO_ASSET);
}

void test_pick_refuses_a_prerelease_a_draft_and_a_bad_tag()
{
    JsonDocument doc;
    Release r;
    TEST_ASSERT_TRUE(pickFrom("{\"tag_name\":\"v1.0.1-rc1\",\"prerelease\":true,\"assets\":[]}", "t2can",
                              r, doc) == Pick::DRAFT);
    TEST_ASSERT_TRUE(pickFrom("{\"tag_name\":\"v1.0.1\",\"draft\":true,\"assets\":[]}", "t2can", r, doc) ==
                     Pick::DRAFT);
    TEST_ASSERT_TRUE(pickFrom("{\"tag_name\":\"nightly\",\"assets\":[]}", "t2can", r, doc) == Pick::NO_TAG);
    TEST_ASSERT_TRUE(pickFrom("{\"message\":\"Not Found\"}", "t2can", r, doc) == Pick::NO_TAG);
}

void test_pick_wants_the_sums_file()
{
    JsonDocument doc;
    Release r;
    TEST_ASSERT_TRUE(pickFrom("{\"tag_name\":\"v1.0.1\",\"assets\":[{\"name\":\"megaopen-t2can-1.0.1.bin\","
                              "\"size\":5,\"browser_download_url\":\"https://x/y.bin\"}]}",
                              "t2can", r, doc) == Pick::NO_SUMS);
}

static const char* SUMS =
    "0000000000000000000000000000000000000000000000000000000000000000  megaopen-t2can-1.0.1-factory.bin\n"
    "AbCdEf0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789  megaopen-t2can-1.0.1.bin\r\n"
    "1111111111111111111111111111111111111111111111111111111111111111 *megaopen-direct-1.0.1.bin\n"
    "2222222222222222222222222222222222222222222222222222222222222222  megaopen-t2can-1.0.1.bin.old";

void test_sum_for_finds_the_exact_name_in_both_forms()
{
    uint8_t h[32];
    TEST_ASSERT_TRUE(sumFor(SUMS, strlen(SUMS), "megaopen-t2can-1.0.1.bin", h));
    TEST_ASSERT_EQUAL_HEX8(0xAB, h[0]);
    TEST_ASSERT_EQUAL_HEX8(0xCD, h[1]);
    TEST_ASSERT_EQUAL_HEX8(0x89, h[31]);
    TEST_ASSERT_TRUE(sumFor(SUMS, strlen(SUMS), "megaopen-direct-1.0.1.bin", h));
    TEST_ASSERT_EQUAL_HEX8(0x11, h[0]);
}

void test_sum_for_does_not_match_a_prefix_or_a_missing_name()
{
    uint8_t h[32];
    TEST_ASSERT_FALSE(sumFor(SUMS, strlen(SUMS), "megaopen-t2can-1.0.1", h));
    TEST_ASSERT_FALSE(sumFor(SUMS, strlen(SUMS), "megaopen-t2can_usbhid-1.0.1.bin", h));
    const char* broken = "zz00000000000000000000000000000000000000000000000000000000000000  a.bin\n";
    TEST_ASSERT_FALSE(sumFor(broken, strlen(broken), "a.bin", h));
    TEST_ASSERT_FALSE(sumFor("", 0, "a.bin", h));
}

void test_trusted_url_takes_githubs_https_hosts_only()
{
    TEST_ASSERT_TRUE(trustedUrl("https://api.github.com/repos/Barbedx/MegaOpen/releases/latest"));
    TEST_ASSERT_TRUE(trustedUrl("https://github.com/Barbedx/MegaOpen/releases/download/v1.0.1/SHA256SUMS"));
    TEST_ASSERT_TRUE(trustedUrl("https://release-assets.githubusercontent.com/github-production-release-asset/1?sp=r&sig=x"));
    TEST_ASSERT_TRUE(trustedUrl("https://objects.githubusercontent.com/a/b"));
    TEST_ASSERT_TRUE(trustedUrl("https://GitHub.com/x"));
    TEST_ASSERT_TRUE(trustedUrl("https://github.com"));
}

void test_trusted_url_refuses_plain_http_lookalikes_and_tricks()
{
    const char* bad[] = {
        "http://github.com/x",                          // in clear
        "https://github.com.evil.example/x",            // a suffix
        "https://evilgithub.com/x",
        "https://raw.githubusercontent.com/x",          // not on the list
        "https://github.com@evil.example/x",            // userinfo
        "https://github.com:8443/x",                    // a port
        "ftp://github.com/x", "github.com/x", "", "https://",
    };
    for (const char* u : bad) TEST_ASSERT_FALSE_MESSAGE(trustedUrl(u), u);
    TEST_ASSERT_FALSE(trustedUrl(nullptr));
}


// GET /releases as release.yml publishes an rc and its predecessor: the rc's
// asset carries X.Y.Z only (MEGAOPEN_VERSION is shared with the final).
static const char* LIST =
    "[{\"tag_name\":\"v1.5.2-rc1\",\"draft\":false,\"prerelease\":true,\"body\":\"x\",\"assets\":["
    "{\"name\":\"megaopen-t2can-1.5.2.bin\",\"size\":1650000,\"browser_download_url\":"
    "\"https://github.com/Barbedx/MegaOpen/releases/download/v1.5.2-rc1/megaopen-t2can-1.5.2.bin\"},"
    "{\"name\":\"SHA256SUMS\",\"size\":600,\"browser_download_url\":"
    "\"https://github.com/Barbedx/MegaOpen/releases/download/v1.5.2-rc1/SHA256SUMS\"}]},"
    "{\"tag_name\":\"v1.6.0\",\"draft\":true,\"prerelease\":false,\"assets\":[]},"
    "{\"tag_name\":\"v1.5.1\",\"draft\":false,\"prerelease\":false,\"assets\":["
    "{\"name\":\"megaopen-t2can-1.5.1.bin\",\"size\":1640000,\"browser_download_url\":"
    "\"https://github.com/Barbedx/MegaOpen/releases/download/v1.5.1/megaopen-t2can-1.5.1.bin\"},"
    "{\"name\":\"SHA256SUMS\",\"size\":600,\"browser_download_url\":"
    "\"https://github.com/Barbedx/MegaOpen/releases/download/v1.5.1/SHA256SUMS\"}]}]";

static Pick newestFrom(const char* json, bool pre, Release& r)
{
    JsonDocument f, doc;
    filterList(f);
    TEST_ASSERT_TRUE(deserializeJson(doc, json, DeserializationOption::Filter(f)) ==
                     DeserializationError::Ok);
    TEST_ASSERT_FALSE(doc[0]["body"].is<const char*>());
    return pickNewest(doc.as<JsonArrayConst>(), "t2can", pre, r);
}

void test_the_test_channel_takes_the_newest_rc_and_its_xyz_asset()
{
    Release r;
    TEST_ASSERT_TRUE(newestFrom(LIST, true, r) == Pick::OK);
    TEST_ASSERT_EQUAL_STRING("v1.5.2-rc1", r.tag);
    TEST_ASSERT_EQUAL_STRING("megaopen-t2can-1.5.2.bin", r.asset);
    TEST_ASSERT_EQUAL(1, r.ver.rc);
    TEST_ASSERT_EQUAL_STRING(
        "https://github.com/Barbedx/MegaOpen/releases/download/v1.5.2-rc1/SHA256SUMS", r.sumsUrl);
}

void test_the_stable_list_skips_the_rc_and_the_draft()
{
    Release r;
    TEST_ASSERT_TRUE(newestFrom(LIST, false, r) == Pick::OK);
    TEST_ASSERT_EQUAL_STRING("v1.5.1", r.tag);
    TEST_ASSERT_TRUE(newestFrom("[]", true, r) == Pick::NO_TAG);
    TEST_ASSERT_TRUE(newestFrom("[{\"tag_name\":\"v2.0.0\",\"draft\":true,\"assets\":[]}]", true, r) ==
                     Pick::DRAFT);
}

void test_newer_tells_rc1_from_rc2_by_the_installed_tag()
{
    // rc1 installed from GitHub: the image says 1.5.2, NVS says v1.5.2-rc1.
    TEST_ASSERT_TRUE(newer(V("v1.5.2-rc2"), "1.5.2", "v1.5.2-rc1"));
    TEST_ASSERT_FALSE(newer(V("v1.5.2-rc1"), "1.5.2", "v1.5.2-rc1"));
    TEST_ASSERT_TRUE(newer(V("v1.5.2"), "1.5.2", "v1.5.2-rc1"));   // the final of the rc
    // Unknown or another version's tag: the image counts as its final.
    TEST_ASSERT_FALSE(newer(V("v1.5.2-rc2"), "1.5.2", nullptr));
    TEST_ASSERT_FALSE(newer(V("v1.5.2-rc2"), "1.5.2", "v1.5.1"));
    TEST_ASSERT_FALSE(newer(V("v1.5.2"), "1.5.2", ""));
    TEST_ASSERT_TRUE(newer(V("v1.5.2-rc1"), "1.5.1", "v1.5.1"));
    TEST_ASSERT_FALSE(newer(V("v1.5.1"), "1.5.2", "v1.5.2-rc1"));
    TEST_ASSERT_TRUE(newer(V("v1.0.0"), "garbage", nullptr));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_accepts_the_tag_and_the_bare_version);
    RUN_TEST(test_parse_rejects_what_is_not_a_version);
    RUN_TEST(test_compare_orders_numbers_not_text);
    RUN_TEST(test_an_rc_is_below_its_final_and_above_the_previous);
    RUN_TEST(test_the_filter_keeps_only_what_pick_reads);
    RUN_TEST(test_pick_takes_this_envs_app_image_not_factory_not_another_env);
    RUN_TEST(test_pick_refuses_a_prerelease_a_draft_and_a_bad_tag);
    RUN_TEST(test_pick_wants_the_sums_file);
    RUN_TEST(test_sum_for_finds_the_exact_name_in_both_forms);
    RUN_TEST(test_sum_for_does_not_match_a_prefix_or_a_missing_name);
    RUN_TEST(test_trusted_url_takes_githubs_https_hosts_only);
    RUN_TEST(test_trusted_url_refuses_plain_http_lookalikes_and_tricks);
    RUN_TEST(test_the_test_channel_takes_the_newest_rc_and_its_xyz_asset);
    RUN_TEST(test_the_stable_list_skips_the_rc_and_the_draft);
    RUN_TEST(test_newer_tells_rc1_from_rc2_by_the_installed_tag);
    return UNITY_END();
}
