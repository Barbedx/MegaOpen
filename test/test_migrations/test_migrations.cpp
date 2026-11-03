// The settings' stored-format steps (src/cfg/Migrations.h), on a map standing
// in for NVS. Each step gets its cases here: what it carries over, that it
// leaves no old key, that it runs once and is safe to run twice.
#include <unity.h>
#include <initializer_list>
#include <map>
#include <string>
#include <stdio.h>
#include <string.h>
#include "cfg/Migrations.h"

using namespace Cfg;

void setUp() {}
void tearDown() {}

namespace
{
    struct Fake : Migrations::Store {
        std::map<std::string, std::string> kv;   // every value as text: "1"/"0" for a Bool
        bool failVersion = false;

        bool has(const char* k) override { return kv.count(k) != 0; }
        bool getBool(const char* k, bool d) override { return has(k) ? kv[k] == "1" : d; }
        uint8_t getU8(const char* k, uint8_t d) override { return has(k) ? (uint8_t)std::stoi(kv[k]) : d; }
        void getStr(const char* k, char* out, size_t cap) override
        {
            snprintf(out, cap, "%s", has(k) ? kv[k].c_str() : "");
        }
        bool putU8(const char* k, uint8_t v) override
        {
            if (failVersion && !strcmp(k, "cfgVer")) return false;
            kv[k] = std::to_string(v);
            return true;
        }
        bool putStr(const char* k, const char* v) override { kv[k] = v; return true; }
        void remove(const char* k) override { kv.erase(k); }
    };

    uint8_t run(Fake& f)
    {
        uint8_t was = 0xEE;
        return Migrations::run(f, was);
    }
}

// A fresh board: nothing to carry, the version is written.
void test_a_fresh_board_gets_the_current_version()
{
    Fake f;
    uint8_t was = 0xEE;
    TEST_ASSERT_EQUAL_UINT8(Migrations::CURRENT, Migrations::run(f, was));
    TEST_ASSERT_EQUAL_UINT8(0, was);
    TEST_ASSERT_EQUAL_STRING(std::to_string(Migrations::CURRENT).c_str(), f.kv["cfgVer"].c_str());
    TEST_ASSERT_FALSE(f.has("panelRole"));
}

// v4: 1.8's funcReg becomes the role and is gone.
void test_v4_funcreg_off_is_the_extension_role()
{
    Fake f;
    f.kv = {{"cfgVer", "3"}, {"funcReg", "0"}};
    TEST_ASSERT_EQUAL_UINT8(4, run(f));
    TEST_ASSERT_EQUAL_STRING("extension", f.kv["panelRole"].c_str());
    TEST_ASSERT_FALSE(f.has("funcReg"));
}

void test_v4_funcreg_on_is_the_emulator_role()
{
    Fake f;
    f.kv = {{"cfgVer", "3"}, {"funcReg", "1"}};
    run(f);
    TEST_ASSERT_EQUAL_STRING("emulator", f.kv["panelRole"].c_str());
}

// A role already stored is the owner's: never overwritten from funcReg.
void test_v4_keeps_a_stored_role()
{
    Fake f;
    f.kv = {{"cfgVer", "3"}, {"funcReg", "1"}, {"panelRole", "extension"}};
    run(f);
    TEST_ASSERT_EQUAL_STRING("extension", f.kv["panelRole"].c_str());
    TEST_ASSERT_FALSE(f.has("funcReg"));
}

// v4 leaves nothing older firmware wrote and this one does not read; today's
// keys stay.
void test_v4_leaves_no_old_key()
{
    Fake f;
    f.kv = {{"cfgVer", "3"}, {"gr1", "80"}, {"gr5", "400"}, {"g1", "80"},
            {"carSig0", "3"}, {"carSigV", "2"}, {"carSigN0", "batt_v"},
            {"decode", "x"}, {"huFmap", "x"}, {"alerts", "x"}, {"macros", "x"}};
    run(f);
    for (const char* k : {"gr1", "gr5", "carSig0", "carSigV", "decode", "huFmap", "alerts", "macros"})
        TEST_ASSERT_FALSE_MESSAGE(f.has(k), k);
    TEST_ASSERT_TRUE(f.has("g1"));
    TEST_ASSERT_TRUE(f.has("carSigN0"));
}

// A board at CURRENT runs nothing, even with an old key present.
void test_a_current_board_runs_nothing()
{
    Fake f;
    f.kv = {{"cfgVer", std::to_string(Migrations::CURRENT)}, {"funcReg", "0"}};
    run(f);
    TEST_ASSERT_FALSE(f.has("panelRole"));
}

// The version write fails: the old version is reported, the steps run again
// next boot and give the same result.
void test_a_failed_version_write_repeats_safely()
{
    Fake f;
    f.kv = {{"cfgVer", "3"}, {"funcReg", "0"}};
    f.failVersion = true;
    TEST_ASSERT_EQUAL_UINT8(3, run(f));
    TEST_ASSERT_EQUAL_STRING("extension", f.kv["panelRole"].c_str());
    f.failVersion = false;
    TEST_ASSERT_EQUAL_UINT8(Migrations::CURRENT, run(f));
    TEST_ASSERT_EQUAL_STRING("extension", f.kv["panelRole"].c_str());
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_fresh_board_gets_the_current_version);
    RUN_TEST(test_v4_funcreg_off_is_the_extension_role);
    RUN_TEST(test_v4_funcreg_on_is_the_emulator_role);
    RUN_TEST(test_v4_keeps_a_stored_role);
    RUN_TEST(test_v4_leaves_no_old_key);
    RUN_TEST(test_a_current_board_runs_nothing);
    RUN_TEST(test_a_failed_version_write_repeats_safely);
    return UNITY_END();
}
