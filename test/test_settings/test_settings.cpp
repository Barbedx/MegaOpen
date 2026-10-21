#include <unity.h>
#include <string.h>
#include <string>
#include <set>
#include "cfg/SettingsRegistry.h"
#include "utils/Log.h"

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
        Settings            s;
        Knobs::Config       k;
        Vehicle::AlertRules a;
        Capture::Settings   c;
        Canbox::Tuning      t;
        bool                msg[Canbox::Rav4Emitter::MAX_MESSAGES];
        Model               m{&s, &k, &a, &c, &t, msg};
        Fixture()
        {
            a.factory();
            for (bool& b : msg) b = true;   // EmitterCfg's default: every message on
        }
    };

    // An entry with one int value: everything but groups, tables and texts.
    bool scalar(const SEntry& e) { return e.type != SType::Group && e.type != SType::Table && e.type != SType::Str; }

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

// POST /api/settings holds one change per entry in a fixed buffer: a request
// that sets every setting at once (a settings import) must fit, and anything
// past the buffer is a 400 (WebUi.cpp), never a silent drop.
void test_one_request_can_change_every_setting()
{
    TEST_ASSERT_TRUE(Registry::count() <= Registry::MAX_CHANGES);
    TEST_ASSERT_TRUE(Registry::MAX_CHANGES <= 255);   // the handler counts in a uint8_t
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
        if (!scalar(e)) continue;
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
        if (!scalar(e) || (e.flags & F_RO)) continue;
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
        if (!scalar(e) || (e.flags & F_RO)) continue;
        Fixture f, ref;
        const int32_t was = Registry::get(f.m, e);
        int32_t v = Registry::stepped(e, was, +1);
        if (v == was) v = Registry::stepped(e, was, -1);
        if (isColor(e) && v == 0) v = 1;
        Registry::set(f.m, e, v);
        for (size_t j = 0; j < Registry::count(); ++j) {
            const SEntry& o = Registry::at(j);
            if (j == i || !scalar(o)) continue;
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
        if (!scalar(e)) continue;
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

// ---- tables: the panel alerts ----------------------------------------------------

namespace
{
    const SEntry& alerts() { return *Registry::find("alerts"); }
    constexpr uint8_t SIG = Vehicle::AlertRules::C_SIG, COND = Vehicle::AlertRules::C_COND,
                      THR = Vehicle::AlertRules::C_THR, CD = Vehicle::AlertRules::C_COOLDOWN,
                      ON = Vehicle::AlertRules::C_ON;
}

void test_tables_are_web_only_and_described()
{
    size_t tables = 0;
    for (size_t i = 0; i < Registry::count(); ++i) {
        const SEntry& e = Registry::at(i);
        if (e.type != SType::Table) {
            TEST_ASSERT_NULL_MESSAGE(e.tab, e.id);
            continue;
        }
        ++tables;
        // The panel has no way to edit a grid.
        TEST_ASSERT_FALSE_MESSAGE(e.flags & F_MENU, e.id);
        TEST_ASSERT_NOT_NULL_MESSAGE(e.tab, e.id);
        TEST_ASSERT_TRUE_MESSAGE(e.tab->rows() > 0 && e.tab->cols > 0, e.id);
        std::set<std::string> ids;
        for (uint8_t c = 0; c < e.tab->cols; ++c) {
            const SColumn& col = e.tab->col[c];
            TEST_ASSERT_TRUE_MESSAGE(ascii(col.id) && *col.id, e.id);
            TEST_ASSERT_TRUE_MESSAGE(ids.insert(col.id).second, col.id);
            TEST_ASSERT_TRUE_MESSAGE(col.type == SType::Bool || col.type == SType::Int || col.type == SType::Enum, col.id);
            TEST_ASSERT_TRUE_MESSAGE(col.type != SType::Enum || col.en, col.id);
            TEST_ASSERT_TRUE_MESSAGE(Registry::colMin(col) <= Registry::colMax(col), col.id);
        }
    }
    TEST_ASSERT_EQUAL(2, tables);   // alerts, huMessages
}

void test_alert_cells_read_the_rules()
{
    Fixture f;
    const SEntry& e = alerts();
    // The factory rule: the car's own low-fuel lamp, on its rising edge.
    TEST_ASSERT_EQUAL_INT32(f.a.r[0].sig, Registry::cell(f.m, e, 0, SIG));
    TEST_ASSERT_EQUAL_STRING("fuel_low", e.tab->col[SIG].en->id((uint8_t)Registry::cell(f.m, e, 0, SIG)));
    TEST_ASSERT_EQUAL_INT32(Vehicle::AC_RISES, Registry::cell(f.m, e, 0, COND));
    TEST_ASSERT_EQUAL_INT32(1, Registry::cell(f.m, e, 0, ON));
    // An empty row is the last enum row, and its id is "".
    const int32_t empty = Registry::cell(f.m, e, 1, SIG);
    TEST_ASSERT_EQUAL_INT32(Registry::colMax(e.tab->col[SIG]), empty);
    TEST_ASSERT_EQUAL_STRING("", e.tab->col[SIG].en->id((uint8_t)empty));
    TEST_ASSERT_EQUAL(empty, Registry::colEnumIndex(e.tab->col[SIG], ""));
    // Out of the grid reads 0.
    TEST_ASSERT_EQUAL_INT32(0, Registry::cell(f.m, e, Vehicle::AlertRules::ROWS, SIG));
    TEST_ASSERT_EQUAL_INT32(0, Registry::cell(f.m, e, 0, Vehicle::AlertRules::COLS));
}

void test_alert_cells_write_through_the_rules_check()
{
    Fixture f;
    const SEntry& e = alerts();
    const Vehicle::Meta& bv = Vehicle::meta(Vehicle::SIG_BATT_V);
    uint8_t ap = 0;
    TEST_ASSERT_TRUE(Registry::setCell(f.m, e, 3, SIG, Vehicle::SIG_BATT_V, &ap));
    TEST_ASSERT_EQUAL(AP_CAR, ap);
    TEST_ASSERT_TRUE(Registry::setCell(f.m, e, 3, COND, Vehicle::AC_BELOW));
    TEST_ASSERT_TRUE(Registry::setCell(f.m, e, 3, THR, bv.min));
    TEST_ASSERT_TRUE(Registry::setCell(f.m, e, 3, CD, 300));
    TEST_ASSERT_TRUE(Registry::setCell(f.m, e, 3, ON, 1));
    TEST_ASSERT_EQUAL(Vehicle::SIG_BATT_V, f.a.r[3].sig);
    TEST_ASSERT_EQUAL(bv.min, f.a.r[3].thr);
    TEST_ASSERT_EQUAL(300, f.a.r[3].cooldownS);
    TEST_ASSERT_TRUE(f.a.r[3].on);
    TEST_ASSERT_EQUAL(bv.decimals, Registry::cellDecimals(f.m, e, 3, THR));
    TEST_ASSERT_EQUAL(0, Registry::cellDecimals(f.m, e, 3, CD));

    // Refused, nothing written: a threshold outside the row's signal, an
    // enum past its rows, a cooldown past 16 bits, a cell off the grid.
    ap = 0;
    TEST_ASSERT_FALSE(Registry::setCell(f.m, e, 3, THR, bv.max + 1, &ap));
    TEST_ASSERT_FALSE(Registry::setCell(f.m, e, 3, COND, Vehicle::AC_COUNT, &ap));
    TEST_ASSERT_FALSE(Registry::setCell(f.m, e, 3, SIG, Vehicle::SIG_COUNT + 1, &ap));
    TEST_ASSERT_FALSE(Registry::setCell(f.m, e, 3, CD, 70000, &ap));
    TEST_ASSERT_FALSE(Registry::setCell(f.m, e, 3, ON, 2, &ap));
    TEST_ASSERT_FALSE(Registry::setCell(f.m, e, Vehicle::AlertRules::ROWS, SIG, 0, &ap));
    TEST_ASSERT_EQUAL(0, ap);
    TEST_ASSERT_EQUAL(bv.min, f.a.r[3].thr);
    TEST_ASSERT_EQUAL(Vehicle::AC_BELOW, f.a.r[3].cond);

    // The same value again is no change.
    TEST_ASSERT_TRUE(Registry::setCell(f.m, e, 3, CD, 300, &ap));
    TEST_ASSERT_EQUAL(0, ap);
}

void test_an_empty_alert_is_never_on()
{
    Fixture f;
    const SEntry& e = alerts();
    TEST_ASSERT_TRUE(Registry::setCell(f.m, e, 5, ON, 1));
    TEST_ASSERT_FALSE(f.a.r[5].on);
}

void test_tables_refuse_the_scalar_calls()
{
    Fixture f;
    const SEntry& e = alerts();
    TEST_ASSERT_FALSE(Registry::valid(e, 0));
    TEST_ASSERT_FALSE(Registry::set(f.m, e, 0));
    TEST_ASSERT_EQUAL_INT32(0, Registry::get(f.m, e));
    // A Model without rules: unavailable, every cell refused.
    Settings s;
    Model m{&s, nullptr};
    TEST_ASSERT_FALSE(Registry::available(m, e));
    TEST_ASSERT_FALSE(Registry::setCell(m, e, 0, CD, 1));
    TEST_ASSERT_EQUAL_INT32(0, Registry::cell(m, e, 0, CD));
    // A scalar entry has no cells.
    TEST_ASSERT_FALSE(Registry::setCell(f.m, *Registry::find("keyPopup"), 0, 0, 1));
}

void test_decimal_text_both_ways()
{
    char b[16];
    TEST_ASSERT_EQUAL_STRING("13.68", Registry::decText(b, sizeof(b), 1368, 2));
    TEST_ASSERT_EQUAL_STRING("-0.05", Registry::decText(b, sizeof(b), -5, 2));
    TEST_ASSERT_EQUAL_STRING("42", Registry::decText(b, sizeof(b), 42, 0));
    int32_t v = 0;
    TEST_ASSERT_TRUE(Registry::decParse(13.68, 2, v));
    TEST_ASSERT_EQUAL_INT32(1368, v);
    TEST_ASSERT_TRUE(Registry::decParse(-0.05, 2, v));
    TEST_ASSERT_EQUAL_INT32(-5, v);
    TEST_ASSERT_FALSE(Registry::decParse(1e12, 0, v));
}

// ---- RAM-only entries ---------------------------------------------------------------

// The log level is set at once and never saved: no apply bit asks anyone to
// write NVS, and it is never a reboot setting.
void test_log_level_is_ram_only()
{
    Fixture f;
    const SEntry* e = Registry::find("logLevel");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->flags & F_VOLATILE);
    TEST_ASSERT_TRUE(e->flags & F_MENU);
    uint8_t ap = 0;
    TEST_ASSERT_TRUE(Registry::set(f.m, *e, Registry::enumIndex(*e, "TRC"), &ap));
    TEST_ASSERT_EQUAL((int)LogLevel::TRC, (int)Log::level());
    TEST_ASSERT_EQUAL(0, ap);
    TEST_ASSERT_TRUE(Registry::set(f.m, *e, Registry::enumIndex(*e, "INF")));
    TEST_ASSERT_EQUAL((int)LogLevel::INF, (int)Log::level());
    for (size_t i = 0; i < Registry::count(); ++i) {
        const SEntry& x = Registry::at(i);
        if (x.flags & F_VOLATILE) TEST_ASSERT_EQUAL_MESSAGE(0, x.apply & AP_REBOOT, x.id);
    }
}

// The panel menu reads a COPY of Settings (PanelMenu's snapshot). A menu-visible
// volatile entry must not live in Settings, or a commit that saves nothing would
// show its old value from that copy: each reads its own owner, so a model over
// a stale copy shows the new value at once.
void test_menu_volatile_entries_read_their_owner()
{
    Fixture f;
    const SEntry* e = Registry::find("logLevel");
    Cfg::Settings stale = f.s;
    Cfg::Model copy{&stale, f.m.k};
    TEST_ASSERT_TRUE(Registry::set(f.m, *e, Registry::enumIndex(*e, "DBG")));
    TEST_ASSERT_EQUAL(Registry::enumIndex(*e, "DBG"), Registry::get(copy, *e));
    Registry::set(f.m, *e, Registry::enumIndex(*e, "INF"));
    // And nothing else the menu can commit is volatile yet; a new one needs
    // the same property, or a Config::touch() test on the board.
    for (size_t i = 0; i < Registry::count(); ++i) {
        const SEntry& x = Registry::at(i);
        if ((x.flags & F_VOLATILE) && (x.flags & F_MENU))
            TEST_ASSERT_EQUAL_STRING_MESSAGE("logLevel", x.id, "a new volatile menu entry: check its snapshot");
    }
}

// The head unit's message switches: one row per message the emitter has,
// RAM only, handed to the hu task.
void test_hu_messages_table()
{
    Fixture f;
    const SEntry* e = Registry::find("huMessages");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->flags & F_VOLATILE);
    TEST_ASSERT_EQUAL(Canbox::Rav4Emitter::messageCount(), e->tab->rows());
    TEST_ASSERT_TRUE(e->tab->rows() <= Canbox::Rav4Emitter::MAX_MESSAGES);
    TEST_ASSERT_EQUAL_INT32(1, Registry::cell(f.m, *e, 2, 0));
    uint8_t ap = 0;
    TEST_ASSERT_TRUE(Registry::setCell(f.m, *e, 2, 0, 0, &ap));
    TEST_ASSERT_FALSE(f.msg[2]);
    TEST_ASSERT_TRUE(f.msg[1] && f.msg[3]);
    TEST_ASSERT_EQUAL(AP_HU, ap);
    TEST_ASSERT_FALSE(Registry::setCell(f.m, *e, 2, 0, 2));
    TEST_ASSERT_FALSE(Registry::setCell(f.m, *e, e->tab->rows(), 0, 1));
    // No emitter: unavailable.
    Settings s;
    Model m{&s, nullptr};
    TEST_ASSERT_FALSE(Registry::available(m, *e));
}

// ---- texts: the canbox version string --------------------------------------------

void test_texts_are_web_only_and_described()
{
    size_t texts = 0;
    for (size_t i = 0; i < Registry::count(); ++i) {
        const SEntry& e = Registry::at(i);
        if (e.type != SType::Str) {
            TEST_ASSERT_NULL_MESSAGE(e.str, e.id);
            continue;
        }
        ++texts;
        TEST_ASSERT_FALSE_MESSAGE(e.flags & F_MENU, e.id);
        TEST_ASSERT_NOT_NULL_MESSAGE(e.str, e.id);
        TEST_ASSERT_TRUE_MESSAGE(e.str->maxLen > 0 && e.str->maxLen < 64, e.id);
    }
    TEST_ASSERT_EQUAL(1, texts);   // raise.version
}

// Exactly 16 bytes on the wire, space-padded; read back trimmed.
void test_version_text_pads_and_refuses()
{
    Fixture f;
    const SEntry* e = Registry::find("raise.version");
    TEST_ASSERT_NOT_NULL(e);
    char b[32];
    uint8_t ap = 0;
    TEST_ASSERT_TRUE(Registry::setText(f.m, *e, "MO 1.2", &ap));
    TEST_ASSERT_EQUAL(AP_HU, ap);
    TEST_ASSERT_EQUAL_STRING("MO 1.2          ", f.t.version);
    Registry::getText(f.m, *e, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("MO 1.2", b);
    ap = 0;
    TEST_ASSERT_TRUE(Registry::setText(f.m, *e, "MO 1.2", &ap));   // unchanged
    TEST_ASSERT_EQUAL(0, ap);
    TEST_ASSERT_TRUE(Registry::setText(f.m, *e, "0123456789ABCDEF"));
    TEST_ASSERT_EQUAL_STRING("0123456789ABCDEF", f.t.version);
    // Refused, nothing written: too long, not printable ASCII, null.
    TEST_ASSERT_FALSE(Registry::setText(f.m, *e, "0123456789ABCDEFG"));
    TEST_ASSERT_FALSE(Registry::setText(f.m, *e, "tab\there"));
    TEST_ASSERT_FALSE(Registry::setText(f.m, *e, "\xD0\x92"));
    TEST_ASSERT_FALSE(Registry::setText(f.m, *e, nullptr));
    TEST_ASSERT_EQUAL_STRING("0123456789ABCDEF", f.t.version);
    // The scalar calls refuse a text; a scalar refuses setText.
    TEST_ASSERT_FALSE(Registry::set(f.m, *e, 0));
    TEST_ASSERT_FALSE(Registry::setText(f.m, *Registry::find("keyPopup"), "x"));
}

// The unit rows store the emitter's own codes, not the row index.
void test_unit_rows_store_the_wire_codes()
{
    Fixture f;
    const SEntry* e = Registry::find("raise.fuelUnit");
    TEST_ASSERT_TRUE(Registry::set(f.m, *e, Registry::enumIndex(*e, "mpg")));
    TEST_ASSERT_EQUAL(0, f.t.fuelUnit);
    TEST_ASSERT_TRUE(Registry::set(f.m, *e, Registry::enumIndex(*e, "l100km")));
    TEST_ASSERT_EQUAL(2, f.t.fuelUnit);
    const SEntry* tr = Registry::find("raise.tripUnit");
    TEST_ASSERT_TRUE(Registry::set(f.m, *tr, Registry::enumIndex(*tr, "hide")));
    TEST_ASSERT_EQUAL(0, f.t.tripUnit);
    TEST_ASSERT_EQUAL_INT32(Registry::enumIndex(*tr, "hide"), Registry::get(f.m, *tr));
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
        if (scalar(e) && !(e.flags & F_VOLATILE) && !(e.apply & (AP_KNOBS | AP_CAR | AP_CAP | AP_HU))) ++n;
    }
    // 40 scalar/enum fields in Settings (carSig counts three), all in the registry.
    TEST_ASSERT_EQUAL(40, n);
    (void)kNotInRegistry;
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_ids_unique_and_findable);
    RUN_TEST(test_one_request_can_change_every_setting);
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
    RUN_TEST(test_tables_are_web_only_and_described);
    RUN_TEST(test_alert_cells_read_the_rules);
    RUN_TEST(test_alert_cells_write_through_the_rules_check);
    RUN_TEST(test_an_empty_alert_is_never_on);
    RUN_TEST(test_tables_refuse_the_scalar_calls);
    RUN_TEST(test_decimal_text_both_ways);
    RUN_TEST(test_log_level_is_ram_only);
    RUN_TEST(test_menu_volatile_entries_read_their_owner);
    RUN_TEST(test_hu_messages_table);
    RUN_TEST(test_texts_are_web_only_and_described);
    RUN_TEST(test_version_text_pads_and_refuses);
    RUN_TEST(test_unit_rows_store_the_wire_codes);
    RUN_TEST(test_settings_struct_unchanged);
    return UNITY_END();
}
