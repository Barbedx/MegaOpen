#include <unity.h>
#include <stdio.h>
#include "utils/LogTags.h"

// The log's tag table (src/utils/LogTags.h): tags register on first use,
// the spec sets per-tag thresholds, before or after a tag appears.

void setUp() {}
void tearDown() {}

void test_tags_register_once_and_fall_back_to_the_global_level()
{
    LogTags t;
    const int8_t hu = t.index("hu");
    TEST_ASSERT_EQUAL_INT8(0, hu);
    TEST_ASSERT_EQUAL_INT8(hu, t.index("hu"));
    TEST_ASSERT_EQUAL_INT8(1, t.index("wifi"));
    TEST_ASSERT_EQUAL_UINT8(2, t.threshold(hu, 2));
    TEST_ASSERT_EQUAL_UINT8(3, t.threshold(-1, 3));        // unknown: the global level
    TEST_ASSERT_EQUAL_INT8(-1, t.index(""));
    TEST_ASSERT_EQUAL_INT8(-1, t.index(nullptr));
}

void test_the_spec_sets_overrides_before_and_after_registration()
{
    LogTags t;
    const int8_t hu = t.index("hu");
    TEST_ASSERT_TRUE(t.setSpec("hu=TRC, knobs=dbg wifi=W"));
    TEST_ASSERT_EQUAL_UINT8(4, t.threshold(hu, 2));
    const int8_t knobs = t.index("knobs");                   // registered after the spec
    TEST_ASSERT_EQUAL_UINT8(3, t.threshold(knobs, 2));
    TEST_ASSERT_EQUAL_UINT8(1, t.threshold(t.index("wifi"), 2));     // the letter form, W
    const int8_t car = t.index("car");
    TEST_ASSERT_EQUAL_UINT8(2, t.threshold(car, 2));         // not named: global
    TEST_ASSERT_TRUE(t.setSpec(""));                          // cleared
    TEST_ASSERT_EQUAL_UINT8(2, t.threshold(hu, 2));
}

void test_a_bad_spec_changes_nothing()
{
    LogTags t;
    const int8_t hu = t.index("hu");
    TEST_ASSERT_TRUE(t.setSpec("hu=DBG"));
    TEST_ASSERT_FALSE(t.setSpec("hu=LOUD"));
    TEST_ASSERT_FALSE(t.setSpec("hu"));
    TEST_ASSERT_FALSE(t.setSpec("=DBG"));
    TEST_ASSERT_EQUAL_UINT8(3, t.threshold(hu, 2));
    TEST_ASSERT_EQUAL_STRING("hu=DBG", t.spec());
}

void test_the_table_is_bounded()
{
    LogTags t;
    static char names[LogTags::MAX + 1][8];
    for (int i = 0; i <= LogTags::MAX; ++i) {
        snprintf(names[i], sizeof(names[i]), "t%d", i);
        const int8_t k = t.index(names[i]);
        if (i < LogTags::MAX) TEST_ASSERT_EQUAL_INT8(i, k);
        else TEST_ASSERT_EQUAL_INT8(-1, k);
    }
}

void test_levels_by_name_and_letter()
{
    TEST_ASSERT_EQUAL_UINT8(0, LogTags::levelOf("err", 3));
    TEST_ASSERT_EQUAL_UINT8(4, LogTags::levelOf("T", 1));
    TEST_ASSERT_EQUAL_UINT8(4, LogTags::levelOf("verbose", 7));
    TEST_ASSERT_EQUAL_UINT8(LogTags::NONE, LogTags::levelOf("info", 4));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_tags_register_once_and_fall_back_to_the_global_level);
    RUN_TEST(test_the_spec_sets_overrides_before_and_after_registration);
    RUN_TEST(test_a_bad_spec_changes_nothing);
    RUN_TEST(test_the_table_is_bounded);
    RUN_TEST(test_levels_by_name_and_letter);
    return UNITY_END();
}
