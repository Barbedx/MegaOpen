#include <unity.h>
#include <string.h>
#include <stdio.h>
#include <string>
#include "ui/MenuEngine.h"
#include "ui/MenuInput.h"
#include "ui/SignalText.h"

// The panel menu without a panel: the tree it builds from the registry and a
// fake car, the cursor, editing, commits, and which keys it takes.

using namespace MenuUi;
using Cfg::Registry::find;

void setUp() {}
void tearDown() {}

namespace
{
    // Two groups: engine with two signals that have data, body with none.
    struct FakeCar {
        bool hasData = true;
        static uint8_t groups(void*) { return 2; }
        static const char* tag(void*, uint8_t g) { return g ? "Body" : "Engine"; }
        static uint8_t signals(void* c, uint8_t g, uint8_t* out, uint8_t cap)
        {
            if (g != 0 || !static_cast<FakeCar*>(c)->hasData) return 0;
            uint8_t n = 0;
            if (n < cap) out[n++] = 7;
            if (n < cap) out[n++] = 9;
            return n;
        }
        static void row(void*, uint8_t sig, char* out, size_t cap)
        {
            snprintf(out, cap, "sig%u          %u", (unsigned)sig, (unsigned)sig * 10);
        }
    };

    struct Fixture {
        Cfg::Settings s;
        Knobs::Config k;
        Cfg::Model    m{&s, &k};
        FakeCar       fc;
        CarView       car;
        Engine        e;
        Fixture()
        {
            car.groups = FakeCar::groups;
            car.groupTag = FakeCar::tag;
            car.signals = FakeCar::signals;
            car.row = FakeCar::row;
            car.ctx = &fc;
            e.begin(&m, &car);
            e.open();
        }
        std::string item(uint8_t i) { char b[COLS + 1]; e.item(i, b, sizeof(b)); return b; }
        // Move the cursor onto the row starting with `text`.
        bool go(const char* text)
        {
            for (uint8_t i = 0; i < e.count(); ++i) {
                if (item(i).rfind(text, 0) == 0) {
                    while (e.cursor() != i) e.key(Cmd::Next);
                    return true;
                }
            }
            return false;
        }
    };

    // A row as the menu lays it out: left text, right text flush with column w.
    std::string pad(const char* l, const char* r, size_t w = COLS - 1)
    {
        std::string s(l);
        s.append(w - strlen(l) - strlen(r), ' ');
        return s + r;
    }

    Keys::Event ev(Keys::Action a, Keys::State st = Keys::State::Press, uint8_t src = 0)
    {
        Keys::Event e;
        e.action = a;
        e.state = st;
        e.srcId = src;
        return e;
    }
}

// ---- the tree ----------------------------------------------------------------------

void test_root_rows()
{
    Fixture f;
    TEST_ASSERT_TRUE(f.e.isOpen());
    TEST_ASSERT_EQUAL(0, f.e.depth());
    TEST_ASSERT_EQUAL(6, f.e.count());
    TEST_ASSERT_EQUAL_STRING("< Exit", f.item(0).c_str());
    TEST_ASSERT_EQUAL_STRING(pad("Display", ">").c_str(), f.item(1).c_str());
    TEST_ASSERT_EQUAL_STRING(pad("Lights", ">").c_str(), f.item(2).c_str());
    TEST_ASSERT_EQUAL_STRING(pad("Service", ">").c_str(), f.item(3).c_str());   // the log level
    TEST_ASSERT_EQUAL_STRING(pad("Car data", ">").c_str(), f.item(4).c_str());
    TEST_ASSERT_EQUAL_STRING("Power off", f.item(5).c_str());
    TEST_ASSERT_EQUAL(1, f.e.cursor());     // opens past "< Exit"
}

void test_exit_row_closes()
{
    Fixture f;
    TEST_ASSERT_TRUE(f.go("< Exit"));
    TEST_ASSERT_EQUAL(Out::Closed, f.e.key(Cmd::Select));
    TEST_ASSERT_FALSE(f.e.isOpen());
}

void test_lights_hidden_without_knobs()
{
    Fixture f;
    Cfg::Model noKnobs{&f.s, nullptr};
    f.e.begin(&noKnobs, &f.car);
    f.e.open();
    TEST_ASSERT_EQUAL(5, f.e.count());
    TEST_ASSERT_TRUE(f.go("Car data"));
}

void test_web_only_settings_stay_out()
{
    Fixture f;
    TEST_ASSERT_FALSE(f.go("Keys"));
    TEST_ASSERT_FALSE(f.go("System"));
    f.go("Display");
    f.e.key(Cmd::Select);
    TEST_ASSERT_EQUAL(1, f.e.depth());
    TEST_ASSERT_EQUAL_STRING("< Back", f.item(0).c_str());
    TEST_ASSERT_TRUE(f.go("Audio screen"));
    TEST_ASSERT_FALSE(f.go("Debounce"));
}

void test_submenu_and_back_keep_the_cursor()
{
    Fixture f;
    f.go("Lights");
    TEST_ASSERT_EQUAL(Out::Redraw, f.e.key(Cmd::Select));
    TEST_ASSERT_TRUE(f.go("Right knob"));
    f.e.key(Cmd::Select);
    TEST_ASSERT_EQUAL(2, f.e.depth());
    TEST_ASSERT_TRUE(f.go("Colour"));
    f.go("< Back");
    f.e.key(Cmd::Select);
    TEST_ASSERT_EQUAL(1, f.e.depth());
    TEST_ASSERT_EQUAL_STRING(pad("Right knob", ">").c_str(), f.item(f.e.cursor()).c_str());
    f.go("< Back");
    f.e.key(Cmd::Select);
    TEST_ASSERT_EQUAL(0, f.e.depth());
    TEST_ASSERT_EQUAL(2, f.e.cursor());     // back on Lights
}

void test_cursor_wraps()
{
    Fixture f;
    f.e.key(Cmd::Prev);                     // onto "< Exit"
    f.e.key(Cmd::Prev);
    TEST_ASSERT_EQUAL(f.e.count() - 1, f.e.cursor());
    f.e.key(Cmd::Next);
    TEST_ASSERT_EQUAL(0, f.e.cursor());
}

// ---- editing ------------------------------------------------------------------------

void test_bool_toggles_at_once()
{
    Fixture f;
    f.go("Display");
    f.e.key(Cmd::Select);
    f.go("Key popup");
    TEST_ASSERT_EQUAL_STRING(pad("Key popup", "On").c_str(), f.item(f.e.cursor()).c_str());
    TEST_ASSERT_EQUAL(Out::Commit, f.e.key(Cmd::Select));
    TEST_ASSERT_EQUAL_PTR(find("keyPopup"), f.e.commitEntry());
    TEST_ASSERT_EQUAL_INT32(0, f.e.commitValue());
    TEST_ASSERT_FALSE(f.e.editing());
}

// Two presses before the owner has stored the first: the second toggles the
// pending value back, it does not repeat the first commit.
void test_bool_pressed_twice_toggles_twice()
{
    Fixture f;
    f.go("Display");
    f.e.key(Cmd::Select);
    f.go("Key popup");
    TEST_ASSERT_EQUAL(Out::Commit, f.e.key(Cmd::Select));
    TEST_ASSERT_EQUAL_INT32(0, f.e.commitValue());
    TEST_ASSERT_EQUAL_STRING(pad("Key popup", "Off").c_str(), f.item(f.e.cursor()).c_str());
    TEST_ASSERT_EQUAL(Out::Commit, f.e.key(Cmd::Select));
    TEST_ASSERT_EQUAL_INT32(1, f.e.commitValue());
    TEST_ASSERT_EQUAL_STRING(pad("Key popup", "On").c_str(), f.item(f.e.cursor()).c_str());
    TEST_ASSERT_EQUAL(Out::Commit, f.e.key(Cmd::Select));
    TEST_ASSERT_EQUAL_INT32(0, f.e.commitValue());
}

void test_int_edit_in_brackets_then_commit()
{
    Fixture f;
    f.go("Display");
    f.e.key(Cmd::Select);
    f.go("Carousel");
    TEST_ASSERT_EQUAL(Out::Redraw, f.e.key(Cmd::Select));
    TEST_ASSERT_TRUE(f.e.editing());
    TEST_ASSERT_EQUAL_STRING(pad("Carousel", "[30s]").c_str(), f.item(f.e.cursor()).c_str());
    f.e.key(Cmd::Next);
    f.e.key(Cmd::Next);
    TEST_ASSERT_EQUAL_STRING(pad("Carousel", "[40s]").c_str(), f.item(f.e.cursor()).c_str());
    // The stored value is untouched until committed.
    TEST_ASSERT_EQUAL(30, f.s.paneCycleS);
    TEST_ASSERT_EQUAL(Out::Commit, f.e.key(Cmd::Select));
    TEST_ASSERT_EQUAL_PTR(find("paneCycleS"), f.e.commitEntry());
    TEST_ASSERT_EQUAL_INT32(40, f.e.commitValue());
    // The menu's copy takes it at once: the row shows what was chosen.
    TEST_ASSERT_EQUAL(40, f.s.paneCycleS);
    TEST_ASSERT_EQUAL_STRING(pad("Carousel", "40s").c_str(), f.item(f.e.cursor()).c_str());
}

void test_edit_back_to_same_value_is_no_commit()
{
    Fixture f;
    f.go("Display");
    f.e.key(Cmd::Select);
    f.go("Text style");
    f.e.key(Cmd::Select);
    f.e.key(Cmd::Next);
    f.e.key(Cmd::Prev);
    TEST_ASSERT_EQUAL(Out::Redraw, f.e.key(Cmd::Select));
}

void test_enum_wraps_while_editing()
{
    Fixture f;
    f.go("Display");
    f.e.key(Cmd::Select);
    f.go("Text style");
    f.e.key(Cmd::Select);
    f.e.key(Cmd::Prev);
    TEST_ASSERT_EQUAL(Out::Commit, f.e.key(Cmd::Select));
    TEST_ASSERT_EQUAL_INT32(Cfg::Registry::maxOf(*find("mediaStyle")), f.e.commitValue());
}

void test_int_stops_at_its_end()
{
    Fixture f;
    f.s.paneSpeed = 6;
    f.go("Display");
    f.e.key(Cmd::Select);
    f.go("Scene speed");
    f.e.key(Cmd::Select);
    TEST_ASSERT_EQUAL(Out::None, f.e.key(Cmd::Next));
}

void test_close_from_inside_an_edit()
{
    Fixture f;
    f.go("Display");
    f.e.key(Cmd::Select);
    f.go("Carousel");
    f.e.key(Cmd::Select);
    f.e.key(Cmd::Next);
    TEST_ASSERT_EQUAL(Out::Closed, f.e.key(Cmd::Close));
    TEST_ASSERT_FALSE(f.e.isOpen());
    TEST_ASSERT_EQUAL(30, f.s.paneCycleS);
    TEST_ASSERT_EQUAL(Out::None, f.e.key(Cmd::Next));
    // Opens at the root again.
    f.e.open();
    TEST_ASSERT_EQUAL(0, f.e.depth());
    TEST_ASSERT_FALSE(f.e.editing());
}

void test_power_off()
{
    Fixture f;
    f.go("Power off");
    TEST_ASSERT_EQUAL(Out::PowerOff, f.e.key(Cmd::Select));
    TEST_ASSERT_FALSE(f.e.isOpen());
}

// ---- the car ------------------------------------------------------------------------

void test_car_groups_with_data_only()
{
    Fixture f;
    f.go("Car data");
    f.e.key(Cmd::Select);
    TEST_ASSERT_EQUAL(2, f.e.count());                // Back + Engine; Body has none
    TEST_ASSERT_EQUAL_STRING(pad("Engine", ">").c_str(), f.item(1).c_str());
    f.go("Engine");
    f.e.key(Cmd::Select);
    TEST_ASSERT_EQUAL(3, f.e.count());
    TEST_ASSERT_EQUAL_STRING("sig7          70", f.item(1).c_str());
    f.go("sig7");
    TEST_ASSERT_EQUAL(Out::None, f.e.key(Cmd::Select));   // read-only
}

void test_refresh_when_data_goes()
{
    Fixture f;
    f.go("Car data");
    f.e.key(Cmd::Select);
    f.go("Engine");
    f.e.key(Cmd::Select);
    f.go("sig9");
    f.fc.hasData = false;
    f.e.refresh();
    TEST_ASSERT_EQUAL(1, f.e.count());
    TEST_ASSERT_EQUAL(0, f.e.cursor());
}

// ---- the screen ---------------------------------------------------------------------

void test_screen_window_and_marker()
{
    Fixture f;
    Engine::Screen s;
    f.e.screen(s);
    TEST_ASSERT_EQUAL_STRING(pad("MENU", "2/6", COLS).c_str(), s.header);
    TEST_ASSERT_EQUAL('>', s.row[0][0]);
    TEST_ASSERT_EQUAL(' ', s.row[1][0]);
    TEST_ASSERT_TRUE(s.more_up);           // "< Exit" above
    TEST_ASSERT_TRUE(s.more_down);
    f.e.key(Cmd::Prev);
    f.e.screen(s);
    TEST_ASSERT_EQUAL_STRING(">< Exit", s.row[0]);
    TEST_ASSERT_FALSE(s.more_up);
    f.e.key(Cmd::Prev);                    // the last row
    f.e.screen(s);
    TEST_ASSERT_EQUAL_STRING(">Power off", s.row[1]);
    TEST_ASSERT_EQUAL(' ', s.row[0][0]);
    TEST_ASSERT_TRUE(s.more_up);
    TEST_ASSERT_FALSE(s.more_down);
    TEST_ASSERT_TRUE(strlen(s.header) <= COLS);
    TEST_ASSERT_TRUE(strlen(s.row[0]) <= COLS);
}

// Every row the menu can show fits the panel and is ASCII.
void test_every_row_fits()
{
    Fixture f;
    auto check = [&]() {
        Engine::Screen s;
        for (uint8_t i = 0; i < f.e.count(); ++i) {
            while (f.e.cursor() != i) f.e.key(Cmd::Next);
            f.e.screen(s);
            for (const char* r : {s.header, s.row[0], s.row[1]}) {
                TEST_ASSERT_TRUE(strlen(r) <= COLS);
                for (const char* p = r; *p; ++p) TEST_ASSERT_TRUE(*p >= 0x20 && *p < 0x7F);
            }
        }
    };
    check();
    for (const char* g : {"Display", "Lights"}) {
        f.e.open();
        f.go(g);
        f.e.key(Cmd::Select);
        check();
    }
}

// ---- the keys ----------------------------------------------------------------------

void test_closed_only_menu_opens()
{
    InputFilter in;
    TEST_ASSERT_EQUAL(Verdict::Do::Open, in.on(ev(Keys::Action::Menu, Keys::State::Press, 4), Mode::Closed).what);
    TEST_ASSERT_EQUAL(Verdict::Do::Swallow, in.on(ev(Keys::Action::Menu, Keys::State::Release, 4), Mode::Open).what);
    for (auto a : {Keys::Action::EncCw, Keys::Action::Ok, Keys::Action::VolUp, Keys::Action::Load})
        TEST_ASSERT_FALSE(in.on(ev(a), Mode::Closed).consumed());
}

void test_open_takes_scroll_and_select_passes_volume()
{
    InputFilter in;
    Verdict v = in.on(ev(Keys::Action::EncCw, Keys::State::Press, 4), Mode::Open);
    TEST_ASSERT_EQUAL(Verdict::Do::Command, v.what);
    TEST_ASSERT_EQUAL(Cmd::Next, v.cmd);
    TEST_ASSERT_EQUAL(Cmd::Prev, in.on(ev(Keys::Action::EncCcw), Mode::Open).cmd);
    TEST_ASSERT_EQUAL(Cmd::Next, in.on(ev(Keys::Action::SrcNext), Mode::Open).cmd);
    TEST_ASSERT_EQUAL(Cmd::Select, in.on(ev(Keys::Action::Ok), Mode::Open).cmd);
    TEST_ASSERT_EQUAL(Cmd::Select, in.on(ev(Keys::Action::Load), Mode::Open).cmd);
    TEST_ASSERT_EQUAL(Cmd::Close, in.on(ev(Keys::Action::Menu), Mode::Open).cmd);
    for (auto a : {Keys::Action::VolUp, Keys::Action::VolDown, Keys::Action::Mute, Keys::Action::Phone})
        TEST_ASSERT_FALSE(in.on(ev(a), Mode::Open).consumed());
}

void test_release_follows_its_press()
{
    InputFilter in;
    // Pressed before the menu opened: its release is not the menu's.
    TEST_ASSERT_FALSE(in.on(ev(Keys::Action::Ok, Keys::State::Release), Mode::Open).consumed());
    in.on(ev(Keys::Action::Ok), Mode::Open);
    TEST_ASSERT_EQUAL(Verdict::Do::Swallow, in.on(ev(Keys::Action::Ok, Keys::State::Long), Mode::Open).what);
    // Released after the menu closed: still the menu's, once.
    TEST_ASSERT_EQUAL(Verdict::Do::Swallow, in.on(ev(Keys::Action::Ok, Keys::State::Release), Mode::Closed).what);
    TEST_ASSERT_FALSE(in.on(ev(Keys::Action::Ok, Keys::State::Release), Mode::Closed).consumed());
}

void test_held_stalk_key_repeats_the_scroll()
{
    InputFilter in;
    in.on(ev(Keys::Action::SrcPrev), Mode::Open);
    Verdict v = in.on(ev(Keys::Action::SrcPrev, Keys::State::Repeat), Mode::Open);
    TEST_ASSERT_EQUAL(Verdict::Do::Command, v.what);
    TEST_ASSERT_EQUAL(Cmd::Prev, v.cmd);
}

void test_panel_off_knob_press_only_powers_on()
{
    InputFilter in;
    TEST_ASSERT_EQUAL(Verdict::Do::PowerOn, in.on(ev(Keys::Action::Ok, Keys::State::Press, 4), Mode::PanelOff).what);
    TEST_ASSERT_EQUAL(Verdict::Do::Swallow, in.on(ev(Keys::Action::Ok, Keys::State::Release, 4), Mode::Closed).what);
    // The left knob's click (Mute) too.
    TEST_ASSERT_EQUAL(Verdict::Do::PowerOn, in.on(ev(Keys::Action::Mute, Keys::State::Press, 4), Mode::PanelOff).what);
    TEST_ASSERT_EQUAL(Verdict::Do::PowerOn, in.on(ev(Keys::Action::Menu, Keys::State::Press, 4), Mode::PanelOff).what);
    // Turns keep their job: volume with the panel off.
    TEST_ASSERT_FALSE(in.on(ev(Keys::Action::VolUp, Keys::State::Press, 4), Mode::PanelOff).consumed());
    TEST_ASSERT_FALSE(in.on(ev(Keys::Action::EncCw, Keys::State::Press, 4), Mode::PanelOff).consumed());
    // The stalk is not a knob.
    TEST_ASSERT_FALSE(in.on(ev(Keys::Action::Ok, Keys::State::Press, 3), Mode::PanelOff).consumed());
}

// ---- one signal as text (the Car data rows and the car screen) ----------------------

void test_signal_value_forms()
{
    char b[24];
    const Vehicle::Meta t{"eng_temp", "C", -40, 215, 0, false, 0};
    MenuUi::signalValue(b, sizeof(b), t, 92, true, true);
    TEST_ASSERT_EQUAL_STRING("92C", b);
    MenuUi::signalValue(b, sizeof(b), t, -7, true, false);
    TEST_ASSERT_EQUAL_STRING("-7C?", b);
    MenuUi::signalValue(b, sizeof(b), t, 92, false, true);
    TEST_ASSERT_EQUAL_STRING("--", b);

    const Vehicle::Meta v{"batt_v", "V", 0, 2000, 2, false, 0};
    MenuUi::signalValue(b, sizeof(b), v, 1412, true, true);
    TEST_ASSERT_EQUAL_STRING("14.12V", b);
    MenuUi::signalValue(b, sizeof(b), v, -5, true, true);
    TEST_ASSERT_EQUAL_STRING("-0.05V", b);

    const Vehicle::Meta d{"door_fl", "", 0, 1, 0, true, 0};
    MenuUi::signalValue(b, sizeof(b), d, 1, true, true);
    TEST_ASSERT_EQUAL_STRING("on", b);
}

void test_signal_row_is_exactly_the_width()
{
    char b[33];
    const Vehicle::Meta t{"eng_temp", "C", -40, 215, 0, false, 0};
    MenuUi::signalRow(b, sizeof(b), 22, t, 92, true, true);
    TEST_ASSERT_EQUAL(22, strlen(b));
    TEST_ASSERT_EQUAL(0, strncmp(b, "eng_temp", 8));
    TEST_ASSERT_EQUAL_STRING("92C", b + 19);
    // A name too long for the value gives way; the value is never cut.
    const Vehicle::Meta l{"a_very_long_signal_name_x", "km", 0, 999999, 0, false, 0};
    MenuUi::signalRow(b, sizeof(b), 22, l, 123456, true, false);
    TEST_ASSERT_EQUAL(22, strlen(b));
    TEST_ASSERT_EQUAL_STRING("123456km?", b + 13);
}

// The car screen's rows: "none" is row 0, a signal is its id + 1, by name.
void test_car_rows_in_the_registry()
{
    Cfg::Settings s;
    Cfg::Model m{&s, nullptr};
    const Cfg::SEntry* e = find("carSig2");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING(Vehicle::meta(Vehicle::SIG_BATT_V).name,
                             Cfg::Registry::enumId(*e, Cfg::Registry::get(m, *e)));
    TEST_ASSERT_TRUE(Cfg::Registry::set(m, *e, 0));
    TEST_ASSERT_EQUAL(Vehicle::SIG_COUNT, s.carSig[1]);
    TEST_ASSERT_EQUAL_STRING("none", Cfg::Registry::enumId(*e, 0));
    const int fuel = Cfg::Registry::enumIndex(*e, "fuel_level");
    TEST_ASSERT_TRUE(Cfg::Registry::set(m, *e, fuel));
    TEST_ASSERT_EQUAL(Vehicle::SIG_FUEL_LEVEL, s.carSig[1]);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_signal_value_forms);
    RUN_TEST(test_signal_row_is_exactly_the_width);
    RUN_TEST(test_car_rows_in_the_registry);
    RUN_TEST(test_root_rows);
    RUN_TEST(test_exit_row_closes);
    RUN_TEST(test_lights_hidden_without_knobs);
    RUN_TEST(test_web_only_settings_stay_out);
    RUN_TEST(test_submenu_and_back_keep_the_cursor);
    RUN_TEST(test_cursor_wraps);
    RUN_TEST(test_bool_toggles_at_once);
    RUN_TEST(test_bool_pressed_twice_toggles_twice);
    RUN_TEST(test_int_edit_in_brackets_then_commit);
    RUN_TEST(test_edit_back_to_same_value_is_no_commit);
    RUN_TEST(test_enum_wraps_while_editing);
    RUN_TEST(test_int_stops_at_its_end);
    RUN_TEST(test_close_from_inside_an_edit);
    RUN_TEST(test_power_off);
    RUN_TEST(test_car_groups_with_data_only);
    RUN_TEST(test_refresh_when_data_goes);
    RUN_TEST(test_screen_window_and_marker);
    RUN_TEST(test_every_row_fits);
    RUN_TEST(test_closed_only_menu_opens);
    RUN_TEST(test_open_takes_scroll_and_select_passes_volume);
    RUN_TEST(test_release_follows_its_press);
    RUN_TEST(test_held_stalk_key_repeats_the_scroll);
    RUN_TEST(test_panel_off_knob_press_only_powers_on);
    return UNITY_END();
}
