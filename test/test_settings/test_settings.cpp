#include <unity.h>
#include <string.h>
#include <string>
#include <set>
#include "cfg/SettingsRegistry.h"

// The registry is what the settings form and the panel menu both draw, so a
// wrong row here is a wrong value in two places. These tests hold the table to
// its contract: every entry reads back what it was given, stays in range, and
// names things the panel can print; and no Settings field goes missing.

using namespace Cfg;

void setUp() {}
void tearDown() {}

namespace
{
    bool ascii(const char* s)
    {
        for (; s && *s; ++s) if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) return false;
        return s != nullptr;
    }

    struct Fixture {
        Settings      s;
        Knobs::Config k;
        Model         m{&s, &k};
    };

    // The values worth trying for one entry: both ends, the middle on the
    // step grid, and every row of an enum.
    std::set<int32_t> probes(const SEntry& e)
    {
        std::set<int32_t> v;
        const int32_t lo = Registry::minOf(e), hi = Registry::maxOf(e);
        if (e.type == SType::Int) {
            v.insert(lo);
            v.insert(hi);
            v.insert(Registry::clamp(e, lo + (hi - lo) / 2));
        } else {
            for (int32_t i = lo; i <= hi; ++i) v.insert(i);
        }
        return v;
    }

    bool isColor(const SEntry& e) { return strstr(e.id, ".color") != nullptr; }
}

// ---- the table itself -----------------------------------------------------------

void test_ids_unique_and_findable()
{
    std::set<std::string> seen;
    for (size_t i = 0; i < Registry::count(); ++i) {
        const SEntry& e = Registry::at(i);
        TEST_ASSERT_TRUE_MESSAGE(seen.insert(e.id).second, e.id);
        TEST_ASSERT_EQUAL_PTR(&e, Registry::find(e.id));
        TEST_ASSERT_EQUAL((int)i, Registry::indexOf(e.id));
    }
    TEST_ASSERT_NULL(Registry::find("noSuchSetting"));
    TEST_ASSERT_EQUAL(-1, Registry::indexOf(nullptr));
}

void test_tags_fit_the_panel()
{
    for (size_t i = 0; i < Registry::count(); ++i) {
        const SEntry& e = Registry::at(i);
        TEST_ASSERT_TRUE_MESSAGE(ascii(e.tag), e.id);
        TEST_ASSERT_TRUE_MESSAGE(strlen(e.tag) <= Registry::TAG_MAX, e.id);
        TEST_ASSERT_TRUE_MESSAGE(e.label && *e.label, e.id);
        if (e.unit) TEST_ASSERT_TRUE_MESSAGE(ascii(e.unit), e.id);
    }
}

void test_parents_are_groups_listed_before_their_children()
{
    for (size_t i = 0; i < Registry::count(); ++i) {
        const SEntry& e = Registry::at(i);
        if (!e.parent) continue;
        const int p = Registry::indexOf(e.parent);
        TEST_ASSERT_TRUE_MESSAGE(p >= 0, e.id);
        TEST_ASSERT_TRUE_MESSAGE(p < (int)i, e.id);
        TEST_ASSERT_TRUE_MESSAGE(Registry::at(p).type == SType::Group, e.id);
        // A child offered in the menu needs a parent that is offered there too.
        if (e.flags & F_MENU) TEST_ASSERT_TRUE_MESSAGE(Registry::at(p).flags & F_MENU, e.id);
    }
}

void test_enums_have_ascii_ids_and_names()
{
    for (size_t i = 0; i < Registry::count(); ++i) {
        const SEntry& e = Registry::at(i);
        if (e.type != SType::Enum) continue;
        TEST_ASSERT_NOT_NULL_MESSAGE(e.en, e.id);
        const uint8_t n = e.en->count();
        TEST_ASSERT_TRUE_MESSAGE(n > 0, e.id);
        std::set<std::string> ids;
        for (uint8_t r = 0; r < n; ++r) {
            TEST_ASSERT_TRUE_MESSAGE(ascii(e.en->id(r)), e.id);
            TEST_ASSERT_TRUE_MESSAGE(*e.en->name(r), e.id);
            TEST_ASSERT_TRUE_MESSAGE(ids.insert(e.en->id(r)).second, e.id);
            TEST_ASSERT_EQUAL(r, Registry::enumIndex(e, e.en->id(r)));
        }
    }
}

void test_ranges_hold_the_defaults()
{
    Fixture f;
    for (size_t i = 0; i < Registry::count(); ++i) {
        const SEntry& e = Registry::at(i);
        if (e.type == SType::Group) continue;
        TEST_ASSERT_TRUE_MESSAGE(Registry::minOf(e) <= Registry::maxOf(e), e.id);
        // A factory board must be inside every range, or the menu opens on
        // a value it could never set.
        TEST_ASSERT_TRUE_MESSAGE(Registry::valid(e, Registry::get(f.m, e)), e.id);
    }
}

// ---- round trips ------------------------------------------------------------------

// Every writable entry reads back every value it accepts, on top of the
// defaults: the web form and the menu cannot drift from the storage.
void test_every_entry_round_trips()
{
    for (size_t i = 0; i < Registry::count(); ++i) {
        const SEntry& e = Registry::at(i);
        if (e.type == SType::Group || (e.flags & F_RO)) continue;
        for (int32_t v : probes(e)) {
            Fixture f;
            if (isColor(e) && v == 0) continue;   // "custom" keeps what is there
            TEST_ASSERT_TRUE_MESSAGE(Registry::set(f.m, e, v), e.id);
            char msg[64];
            snprintf(msg, sizeof(msg), "%s = %ld", e.id, (long)v);
            TEST_ASSERT_EQUAL_INT32_MESSAGE(v, Registry::get(f.m, e), msg);
        }
    }
}

// Writing one entry leaves every other entry as it was (no aliasing).
void test_entries_are_independent()
{
    for (size_t i = 0; i < Registry::count(); ++i) {
        const SEntry& e = Registry::at(i);
        if (e.type == SType::Group || (e.flags & F_RO)) continue;
        Fixture f, ref;
        const int32_t was = Registry::get(f.m, e);
        int32_t v = Registry::stepped(e, was, +1);
        if (v == was) v = Registry::stepped(e, was, -1);
        if (isColor(e) && v == 0) v = 1;
        Registry::set(f.m, e, v);
        for (size_t j = 0; j < Registry::count(); ++j) {
            const SEntry& o = Registry::at(j);
            if (j == i || o.type == SType::Group) continue;
            TEST_ASSERT_EQUAL_INT32_MESSAGE(Registry::get(ref.m, o), Registry::get(f.m, o), o.id);
        }
    }
}

// String fields stored by id: an id this build does not know reads as row 0
// and is replaced only when written.
void test_unknown_stored_id_reads_as_row_zero()
{
    Fixture f;
    f.s.mediaStyle = "gone-style";
    const SEntry* e = Registry::find("mediaStyle");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT32(0, Registry::get(f.m, *e));
    TEST_ASSERT_EQUAL_STRING("gone-style", f.s.mediaStyle.c_str());
    TEST_ASSERT_TRUE(Registry::set(f.m, *e, 1));
    TEST_ASSERT_EQUAL_STRING(e->en->id(1), f.s.mediaStyle.c_str());
}

void test_pane_auto_is_its_own_row()
{
    Fixture f;
    const SEntry* e = Registry::find("mediaPane");
    const int a = Registry::enumIndex(*e, "auto");
    TEST_ASSERT_TRUE(a > 0);
    TEST_ASSERT_TRUE(Registry::set(f.m, *e, a));
    TEST_ASSERT_EQUAL_STRING("auto", f.s.mediaPane.c_str());
    TEST_ASSERT_TRUE(Registry::set(f.m, *e, 0));
    TEST_ASSERT_EQUAL_STRING("off", f.s.mediaPane.c_str());
}

void test_splash_under_a_second_becomes_one()
{
    Fixture f;
    const SEntry* e = Registry::find("trackSplashMs");
    TEST_ASSERT_TRUE(Registry::set(f.m, *e, 300));
    TEST_ASSERT_EQUAL(1000, f.s.trackSplashMs);
    TEST_ASSERT_TRUE(Registry::set(f.m, *e, 0));
    TEST_ASSERT_EQUAL(0, f.s.trackSplashMs);
}

void test_color_preset_paints_the_zone_and_custom_keeps_it()
{
    Fixture f;
    const SEntry* e = Registry::find("ledR.color");
    const int red = Registry::enumIndex(*e, "red");
    TEST_ASSERT_TRUE(Registry::set(f.m, *e, red));
    for (const auto& l : f.k.zone[Knobs::ZONE_RIGHT].led) {
        TEST_ASSERT_EQUAL(255, l.r);
        TEST_ASSERT_EQUAL(0, l.g);
    }
    TEST_ASSERT_EQUAL_INT32(red, Registry::get(f.m, *e));
    // One LED changed on the web: the zone is no preset any more.
    f.k.zone[Knobs::ZONE_RIGHT].count = 5;
    f.k.zone[Knobs::ZONE_RIGHT].led[3] = {1, 2, 3};
    TEST_ASSERT_EQUAL_INT32(0, Registry::get(f.m, *e));
    TEST_ASSERT_TRUE(Registry::set(f.m, *e, 0));
    TEST_ASSERT_EQUAL(1, f.k.zone[Knobs::ZONE_RIGHT].led[3].r);
    // The left zone was not touched.
    TEST_ASSERT_EQUAL_INT32(Registry::get(Fixture{}.m, *Registry::find("ledL.color")),
                            Registry::get(f.m, *Registry::find("ledL.color")));
}

// ---- refusals and apply bits -------------------------------------------------------

void test_refusals_write_nothing()
{
    Fixture f;
    uint8_t ap = 0;
    const SEntry* cyc = Registry::find("paneCycleS");
    TEST_ASSERT_FALSE(Registry::set(f.m, *cyc, 4, &ap));
    TEST_ASSERT_FALSE(Registry::set(f.m, *cyc, 601, &ap));
    TEST_ASSERT_EQUAL(30, f.s.paneCycleS);

    const SEntry* det = Registry::find("can2Detected");
    TEST_ASSERT_FALSE(Registry::set(f.m, *det, 1, &ap));
    TEST_ASSERT_FALSE(Registry::set(f.m, *Registry::find("display"), 0, &ap));
    TEST_ASSERT_EQUAL(0, ap);
}

void test_knob_entries_unavailable_without_knobs()
{
    Settings s;
    Model m{&s, nullptr};
    const SEntry* e = Registry::find("ledBrightness");
    TEST_ASSERT_FALSE(Registry::available(m, *e));
    TEST_ASSERT_FALSE(Registry::set(m, *e, 10));
    TEST_ASSERT_EQUAL_INT32(0, Registry::get(m, *e));
    char b[16];
    Registry::format(m, *e, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("--", b);
    TEST_ASSERT_TRUE(Registry::available(m, *Registry::find("keyPopup")));
}

void test_apply_bits_only_on_change()
{
    Fixture f;
    uint8_t ap = 0;
    const SEntry* kp = Registry::find("keyPopup");
    TEST_ASSERT_TRUE(Registry::set(f.m, *kp, 1, &ap));      // already on
    TEST_ASSERT_EQUAL(0, ap);
    TEST_ASSERT_TRUE(Registry::set(f.m, *kp, 0, &ap));
    TEST_ASSERT_EQUAL(AP_UI, ap);
    TEST_ASSERT_TRUE(Registry::set(f.m, *Registry::find("bleMode"), 0, &ap));
    TEST_ASSERT_EQUAL(AP_UI | AP_REBOOT, ap);
    TEST_ASSERT_TRUE(Registry::set(f.m, *Registry::find("ledEnabled"), 1, &ap));
    TEST_ASSERT_EQUAL(AP_UI | AP_REBOOT | AP_KNOBS, ap);
}

// ---- the menu's helpers -----------------------------------------------------------

void test_stepped()
{
    const SEntry* cyc = Registry::find("paneCycleS");
    TEST_ASSERT_EQUAL_INT32(35, Registry::stepped(*cyc, 30, +1));
    TEST_ASSERT_EQUAL_INT32(600, Registry::stepped(*cyc, 600, +1));   // stops
    TEST_ASSERT_EQUAL_INT32(5, Registry::stepped(*cyc, 5, -1));

    const SEntry* b = Registry::find("autoTime");
    TEST_ASSERT_EQUAL_INT32(0, Registry::stepped(*b, 1, +1));         // flips
    TEST_ASSERT_EQUAL_INT32(1, Registry::stepped(*b, 0, -1));

    const SEntry* st = Registry::find("mediaStyle");
    const int32_t last = Registry::maxOf(*st);
    TEST_ASSERT_EQUAL_INT32(0, Registry::stepped(*st, last, +1));     // wraps
    TEST_ASSERT_EQUAL_INT32(last, Registry::stepped(*st, 0, -1));
}

void test_format_is_ascii_and_short()
{
    Fixture f;
    char b[27];
    for (size_t i = 0; i < Registry::count(); ++i) {
        const SEntry& e = Registry::at(i);
        if (e.type == SType::Group) continue;
        Registry::format(f.m, e, b, sizeof(b));
        TEST_ASSERT_TRUE_MESSAGE(ascii(b) && *b, e.id);
    }
    Registry::format(f.m, *Registry::find("keyPopupMs"), b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("2000ms", b);
    Registry::format(f.m, *Registry::find("keyPopup"), b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("On", b);
    Registry::format(f.m, *Registry::find("keyPopupMs"), b, 3);
    TEST_ASSERT_EQUAL_STRING("20", b);
}

// ---- drift guard --------------------------------------------------------------------

// Settings fields with no registry entry, on purpose: strings the panel
// cannot edit, which keep their own web handling.
static const char* const kNotInRegistry[] = {"ssid", "pass", "mdnsName", "apName", "apPass"};

// A new field in Cfg::Settings changes its size and lands here: give it a
// registry entry (or add it to kNotInRegistry with the reason), then update
// the number. Host layout: std::string fields.
void test_settings_struct_unchanged()
{
    TEST_ASSERT_EQUAL_MESSAGE(sizeof(std::string) * 11 + 80, sizeof(Settings),
                              "Cfg::Settings changed: add a registry entry, then update this size");
    // Every other field: count the entries that write Settings.
    size_t n = 0;
    for (size_t i = 0; i < Registry::count(); ++i) {
        const SEntry& e = Registry::at(i);
        if (e.type != SType::Group && !(e.apply & AP_KNOBS)) ++n;
    }
    // 40 scalar/enum fields in Settings (carSig counts three), all in the registry.
    TEST_ASSERT_EQUAL(40, n);
    (void)kNotInRegistry;
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_ids_unique_and_findable);
    RUN_TEST(test_tags_fit_the_panel);
    RUN_TEST(test_parents_are_groups_listed_before_their_children);
    RUN_TEST(test_enums_have_ascii_ids_and_names);
    RUN_TEST(test_ranges_hold_the_defaults);
    RUN_TEST(test_every_entry_round_trips);
    RUN_TEST(test_entries_are_independent);
    RUN_TEST(test_unknown_stored_id_reads_as_row_zero);
    RUN_TEST(test_pane_auto_is_its_own_row);
    RUN_TEST(test_splash_under_a_second_becomes_one);
    RUN_TEST(test_color_preset_paints_the_zone_and_custom_keeps_it);
    RUN_TEST(test_refusals_write_nothing);
    RUN_TEST(test_knob_entries_unavailable_without_knobs);
    RUN_TEST(test_apply_bits_only_on_change);
    RUN_TEST(test_stepped);
    RUN_TEST(test_format_is_ascii_and_short);
    RUN_TEST(test_settings_struct_unchanged);
    return UNITY_END();
}
