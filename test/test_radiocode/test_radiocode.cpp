#include <unity.h>
#include <string.h>
#include <stdint.h>
#include "display/RadioCode.h"

// The stock radio's code, typed for the owner (src/display/RadioCode.h):
// the frames are the car's captures (MeganeCAN notes/AFFA3_SCREENS.md), the
// keys are what the panel task would put on the bus.
using MegaOpen::RadioCode;

namespace
{
    struct Sent { uint16_t wire; bool hold; };
    Sent g_sent[64];
    int  g_n = 0;
    bool g_bus = true;
    bool send(uint16_t w, bool h, void*)
    {
        if (!g_bus) return false;
        if (g_n < 64) g_sent[g_n] = Sent{w, h};
        ++g_n;
        return true;
    }
    const uint8_t PROMPT[8] = {0x21, 0x20, 0x20, 'C', 'O', 'D', 'E', 0x20};
    const uint8_t FIELD0[8] = {0x21, 0x20, 0x20, 0xB0, '0', '0', '0', 0x20};
    const uint8_t FIELD1[8] = {0x21, 0x20, 0x20, '5', 0xB1, '0', '0', 0x20};
    const uint8_t OTHER[8]  = {0x21, 0x20, 0x20, 'A', 'U', 'X', 0x20, 0x20};

    void run(RadioCode& rc, uint32_t from, uint32_t to)
    {
        for (uint32_t t = from; t <= to; t += 10) rc.tick(t);
    }
    RadioCode armed(const char* code, uint32_t at = 1000)
    {
        RadioCode rc;
        rc.begin(send, nullptr);
        rc.setCode(code);
        rc.onRx(RadioCode::ID_TEXT, PROMPT, 8, at);
        return rc;
    }
    // The whole attempt, key by key, until the last one is out (the state
    // is then Verdict), or 20 s when it never gets there: the time reached.
    uint32_t typeAll(RadioCode& rc, uint32_t from)
    {
        uint32_t t = from;
        for (; t < from + 20000 && rc.view().state != RadioCode::State::Verdict; t += 10) rc.tick(t);
        return t;
    }
}

void setUp() { g_n = 0; g_bus = true; }
void tearDown() {}

void test_the_prompt_is_the_captured_frames()
{
    TEST_ASSERT_TRUE(RadioCode::promptFrame(PROMPT, 8));
    TEST_ASSERT_TRUE(RadioCode::fieldFrame(FIELD0, 8));
    TEST_ASSERT_TRUE(RadioCode::fieldFrame(FIELD1, 8));
    TEST_ASSERT_FALSE(RadioCode::promptFrame(OTHER, 8));
    TEST_ASSERT_FALSE(RadioCode::fieldFrame(OTHER, 8));
    const uint8_t first[8] = {0x10, 0x0E, 0x77, 0x55, 0x55, 0xFF, 0x60, 0x01};
    TEST_ASSERT_FALSE(RadioCode::promptFrame(first, 8));
    TEST_ASSERT_FALSE(RadioCode::fieldFrame(first, 8));
    TEST_ASSERT_FALSE(RadioCode::promptFrame(PROMPT, 7));
}

void test_the_prompt_types_the_code_in_order()
{
    RadioCode rc = armed("5321");
    TEST_ASSERT_EQUAL((int)RadioCode::Event::Prompt, (int)rc.takeEvent());
    rc.tick(1000);
    TEST_ASSERT_EQUAL_MESSAGE(0, g_n, "the screen settles first");
    rc.tick(1000 + RadioCode::START_MS - 10);
    TEST_ASSERT_EQUAL(0, g_n);
    typeAll(rc, 1000);
    // 5 up, Load; 3 up, Load; 2 up, Load; 1 up, Load held.
    const int want = 5 + 1 + 3 + 1 + 2 + 1 + 1 + 1;
    TEST_ASSERT_EQUAL(want, g_n);
    int i = 0;
    const uint8_t digits[4] = {5, 3, 2, 1};
    for (int p = 0; p < 4; ++p) {
        for (int k = 0; k < digits[p]; ++k) {
            TEST_ASSERT_EQUAL_HEX16(RadioCode::KEY_ROLL_UP, g_sent[i].wire);
            TEST_ASSERT_FALSE(g_sent[i].hold);
            ++i;
        }
        TEST_ASSERT_EQUAL_HEX16(RadioCode::KEY_LOAD, g_sent[i].wire);
        TEST_ASSERT_EQUAL(p == 3, g_sent[i].hold);
        ++i;
    }
    TEST_ASSERT_EQUAL((int)RadioCode::Event::Typed, (int)rc.takeEvent());
    TEST_ASSERT_EQUAL((int)RadioCode::State::Verdict, (int)rc.view().state);
    TEST_ASSERT_EQUAL(1, rc.view().attempts);
    TEST_ASSERT_EQUAL(want, rc.view().sent);
}

void test_the_keys_are_spaced()
{
    RadioCode rc = armed("1111");
    // Every pass at once: still one key per KEY_GAP_MS.
    const uint32_t t0 = 1000 + RadioCode::START_MS;
    for (int k = 0; k < 50; ++k) rc.tick(t0);
    TEST_ASSERT_EQUAL(1, g_n);
    for (int k = 0; k < 50; ++k) rc.tick(t0 + RadioCode::KEY_GAP_MS - 1);
    TEST_ASSERT_EQUAL(1, g_n);
    rc.tick(t0 + RadioCode::KEY_GAP_MS);
    TEST_ASSERT_EQUAL(2, g_n);
}

void test_a_zero_digit_needs_no_click()
{
    RadioCode rc = armed("0000");
    typeAll(rc, 1000);
    TEST_ASSERT_EQUAL(4, g_n);
    for (int i = 0; i < 4; ++i) TEST_ASSERT_EQUAL_HEX16(RadioCode::KEY_LOAD, g_sent[i].wire);
    TEST_ASSERT_FALSE(g_sent[2].hold);
    TEST_ASSERT_TRUE(g_sent[3].hold);
}

void test_silence_after_the_keys_is_accepted()
{
    RadioCode rc = armed("5321");
    const uint32_t end = typeAll(rc, 1000);
    (void)rc.takeEvent();
    // The echo of the last key is not the radio's answer.
    rc.onRx(RadioCode::ID_TEXT, FIELD1, 8, end + 100);
    run(rc, end, end + RadioCode::RESULT_MS + 100);
    TEST_ASSERT_EQUAL((int)RadioCode::Event::Accepted, (int)rc.takeEvent());
    TEST_ASSERT_EQUAL((int)RadioCode::State::Idle, (int)rc.view().state);
    TEST_ASSERT_EQUAL(1, rc.view().accepted);
    TEST_ASSERT_EQUAL(0, rc.view().rejected);
}

void test_the_prompt_back_is_rejected_and_the_next_prompt_waits_out_the_cooldown()
{
    RadioCode rc = armed("5321");
    const uint32_t end = typeAll(rc, 1000);
    (void)rc.takeEvent();
    const int typed = g_n;
    run(rc, end, end + 1000);
    rc.onRx(RadioCode::ID_TEXT, PROMPT, 8, end + 1500);   // "CODE" again: wrong code
    rc.tick(end + 1510);
    TEST_ASSERT_EQUAL((int)RadioCode::Event::Rejected, (int)rc.takeEvent());
    TEST_ASSERT_EQUAL(1, rc.view().rejected);
    TEST_ASSERT_EQUAL((int)RadioCode::State::Idle, (int)rc.view().state);
    // The field again a second later: not typed again (the radio locks up
    // longer after each wrong code), said once.
    rc.onRx(RadioCode::ID_TEXT, FIELD0, 8, end + 2500);
    TEST_ASSERT_EQUAL((int)RadioCode::Event::Skipped, (int)rc.takeEvent());
    rc.onRx(RadioCode::ID_TEXT, FIELD0, 8, end + 2600);
    TEST_ASSERT_EQUAL((int)RadioCode::Event::None, (int)rc.takeEvent());
    run(rc, end + 2500, end + 6000);
    TEST_ASSERT_EQUAL(typed, g_n);
    // After the cooldown: once more.
    const uint32_t later = 1000 + RadioCode::START_MS + RadioCode::COOLDOWN_MS + 10;
    rc.onRx(RadioCode::ID_TEXT, PROMPT, 8, later);
    TEST_ASSERT_EQUAL((int)RadioCode::Event::Prompt, (int)rc.takeEvent());
    typeAll(rc, later);
    TEST_ASSERT_EQUAL(2 * typed, g_n);
    TEST_ASSERT_EQUAL(2, rc.view().attempts);
}

void test_a_bad_or_empty_setting_is_off()
{
    RadioCode rc;
    rc.begin(send, nullptr);
    rc.setCode("12a4");
    TEST_ASSERT_FALSE(rc.enabled());
    rc.setCode("123");
    TEST_ASSERT_FALSE(rc.enabled());
    rc.setCode("");
    TEST_ASSERT_FALSE(rc.enabled());
    TEST_ASSERT_EQUAL((int)RadioCode::State::Off, (int)rc.view().state);
    rc.onRx(RadioCode::ID_TEXT, PROMPT, 8, 1000);
    typeAll(rc, 1000);
    TEST_ASSERT_EQUAL(0, g_n);
    TEST_ASSERT_EQUAL((int)RadioCode::Event::None, (int)rc.takeEvent());
    // Set later (the page): on from the next prompt.
    rc.setCode("5321");
    TEST_ASSERT_TRUE(rc.enabled());
    TEST_ASSERT_EQUAL_STRING("5321", rc.code());
    rc.onRx(RadioCode::ID_TEXT, PROMPT, 8, 2000);
    typeAll(rc, 2000);
    TEST_ASSERT_EQUAL(15, g_n);
}

void test_a_bus_that_refuses_every_key_gives_up()
{
    RadioCode rc = armed("5321");
    g_bus = false;
    typeAll(rc, 1000);
    TEST_ASSERT_EQUAL(0, g_n);
    TEST_ASSERT_EQUAL((int)RadioCode::Event::NoBus, (int)rc.takeEvent());
    TEST_ASSERT_EQUAL((int)RadioCode::State::Idle, (int)rc.view().state);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_prompt_is_the_captured_frames);
    RUN_TEST(test_the_prompt_types_the_code_in_order);
    RUN_TEST(test_the_keys_are_spaced);
    RUN_TEST(test_a_zero_digit_needs_no_click);
    RUN_TEST(test_silence_after_the_keys_is_accepted);
    RUN_TEST(test_the_prompt_back_is_rejected_and_the_next_prompt_waits_out_the_cooldown);
    RUN_TEST(test_a_bad_or_empty_setting_is_off);
    RUN_TEST(test_a_bus_that_refuses_every_key_gives_up);
    return UNITY_END();
}
