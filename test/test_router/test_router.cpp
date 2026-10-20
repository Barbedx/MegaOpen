// Which source reaches the glass.
//
// This is the component the whole "media info from the iPhone" feature hangs
// off, and it had no test at all. Its failures are not crashes — they are a
// panel that shows the wrong thing, or nothing, while every counter on every
// diagnostic page reads healthy. That is the expensive kind.
//
// Two behaviours are worth pinning down and neither is obvious:
//
//   * what `auto` picks, and WHEN it changes its mind. The head unit wins while
//     it is live because it is what is actually coming out of the speakers; the
//     phone reports whatever its media app last touched, including a track
//     paused an hour ago in another car.
//   * what happens when a slot was never registered. A board with no BLE radio
//     compiled in has a null AMS slot, and `mediaSource=ams` on it must be a
//     quiet "nothing to show" rather than a null dereference.
#include <unity.h>
#include <string.h>

#include "media/MediaRouter.h"

void setUp() {}
void tearDown() {}

// A source that is whatever the test needs it to be.
class FakeSource : public IMediaSource {
public:
    FakeSource(const char* n, bool live) : _name(n), _live(live) {}

    const MediaInfo& current() const override { return _info; }
    bool active() const override { return _live; }
    const char* statusText() const override { return _status; }
    const char* name() const override { return _name; }

    void setLive(bool v) { _live = v; }
    void setTitle(const char* t) { _info.title = t; }
    void setStatus(const char* s) { _status = s; }

    // Proves snapshot() is forwarded rather than reimplemented: the router must
    // hand back what the SOURCE decided to publish, not a copy of current().
    MediaInfo snapshot() const override {
        MediaInfo m = _info;
        m.title += "|snap";
        return m;
    }

    // The bounded read the panel task uses; `busy` plays a held mutex.
    bool trySnapshot(MediaInfo& out, uint32_t) const override {
        if (_busy) return false;
        out = snapshot();
        return true;
    }
    void setBusy(bool b) { _busy = b; }

private:
    MediaInfo _info;
    const char* _name;
    bool _live;
    const char* _status = "";
    bool _busy = false;
};

// ---- the mode string, which is what NVS and the web UI store ---------------

static void test_mode_strings_round_trip()
{
    TEST_ASSERT_EQUAL(MediaRouter::Mode::Ams,  MediaRouter::modeFromStr("ams"));
    TEST_ASSERT_EQUAL(MediaRouter::Mode::Hu,   MediaRouter::modeFromStr("hu"));
    TEST_ASSERT_EQUAL(MediaRouter::Mode::Auto, MediaRouter::modeFromStr("auto"));
    TEST_ASSERT_EQUAL_STRING("ams",  MediaRouter::modeStr(MediaRouter::Mode::Ams));
    TEST_ASSERT_EQUAL_STRING("hu",   MediaRouter::modeStr(MediaRouter::Mode::Hu));
    TEST_ASSERT_EQUAL_STRING("auto", MediaRouter::modeStr(MediaRouter::Mode::Auto));
}

// Junk must land on the PROVEN source, not on whichever enum value happens to be
// zero by accident. Ams is both the default and the path that works.
static void test_junk_and_null_fall_back_to_ams()
{
    TEST_ASSERT_EQUAL(MediaRouter::Mode::Ams, MediaRouter::modeFromStr("nonsense"));
    TEST_ASSERT_EQUAL(MediaRouter::Mode::Ams, MediaRouter::modeFromStr(""));
    TEST_ASSERT_EQUAL(MediaRouter::Mode::Ams, MediaRouter::modeFromStr(nullptr));
}

// ---- explicit modes --------------------------------------------------------

static void test_explicit_modes_pick_their_own_source_even_when_dead()
{
    FakeSource ams("ams", false), hu("hu", true);
    MediaRouter r;
    r.setSources(&ams, &hu);

    // AMS with no phone still renders ITS status line — "no phone connected" is
    // information, and silently showing the head unit instead would be a screen
    // that lies about where it came from.
    r.setMode(MediaRouter::Mode::Ams);
    TEST_ASSERT_EQUAL_PTR(&ams, r.activeSource());
    TEST_ASSERT_FALSE(r.active());
    TEST_ASSERT_EQUAL_STRING("ams", r.sourceName());

    r.setMode(MediaRouter::Mode::Hu);
    TEST_ASSERT_EQUAL_PTR(&hu, r.activeSource());
    TEST_ASSERT_TRUE(r.active());
    TEST_ASSERT_EQUAL_STRING("hu", r.sourceName());
}

// ---- auto ------------------------------------------------------------------

static void test_auto_prefers_the_head_unit_while_it_is_live()
{
    FakeSource ams("ams", true), hu("hu", true);
    MediaRouter r;
    r.setSources(&ams, &hu);
    r.setMode(MediaRouter::Mode::Auto);
    TEST_ASSERT_EQUAL_PTR(&hu, r.activeSource());
}

static void test_auto_falls_back_to_the_phone_when_the_head_unit_goes_quiet()
{
    FakeSource ams("ams", true), hu("hu", true);
    MediaRouter r;
    r.setSources(&ams, &hu);
    r.setMode(MediaRouter::Mode::Auto);
    TEST_ASSERT_EQUAL_PTR(&hu, r.activeSource());

    hu.setLive(false);
    TEST_ASSERT_EQUAL_PTR(&ams, r.activeSource());

    // ...and hands it back the moment the head unit speaks again. The pick is
    // re-evaluated per call rather than latched, which is what lets the screen
    // follow whichever box is actually playing.
    hu.setLive(true);
    TEST_ASSERT_EQUAL_PTR(&hu, r.activeSource());
}

static void test_auto_with_no_head_unit_registered_is_just_the_phone()
{
    FakeSource ams("ams", true);
    MediaRouter r;
    r.setSources(&ams, nullptr);
    r.setMode(MediaRouter::Mode::Auto);
    TEST_ASSERT_EQUAL_PTR(&ams, r.activeSource());
}

// ---- unregistered slots ----------------------------------------------------
// A build with no BLE radio has a null AMS slot. Selecting it must be a quiet
// "nothing to show", not a crash on a dashboard.

static void test_a_null_slot_is_answered_and_not_dereferenced()
{
    MediaRouter r;
    r.setSources(nullptr, nullptr);
    r.setMode(MediaRouter::Mode::Ams);

    TEST_ASSERT_NULL(r.activeSource());
    TEST_ASSERT_FALSE(r.active());
    TEST_ASSERT_EQUAL_STRING("none", r.sourceName());
    TEST_ASSERT_EQUAL_STRING("no media source", r.statusText());
    // An empty MediaInfo, not a wild reference.
    TEST_ASSERT_TRUE(r.current().title.empty());
    TEST_ASSERT_TRUE(r.snapshot().title.empty());
}

static void test_selecting_the_missing_half_of_a_pair_is_still_safe()
{
    FakeSource ams("ams", true);
    MediaRouter r;
    r.setSources(&ams, nullptr);
    r.setMode(MediaRouter::Mode::Hu);          // the slot that was never filled
    TEST_ASSERT_NULL(r.activeSource());
    TEST_ASSERT_FALSE(r.active());
    TEST_ASSERT_TRUE(r.current().title.empty());
}

// ---- forwarding ------------------------------------------------------------

static void test_the_router_forwards_rather_than_reimplements()
{
    FakeSource ams("ams", true), hu("hu", true);
    ams.setTitle("Phone Track");
    ams.setStatus("phone ok");
    hu.setTitle("HU Track");
    MediaRouter r;
    r.setSources(&ams, &hu);
    r.setMode(MediaRouter::Mode::Ams);

    TEST_ASSERT_EQUAL_STRING("Phone Track", r.current().title.c_str());
    TEST_ASSERT_EQUAL_STRING("phone ok", r.statusText());
    // snapshot() must go through the SOURCE's override — the whole reason it is
    // virtual is that a source written from another task returns a locked copy,
    // and a router that returned current() instead would hand a consumer the
    // very race the interface exists to prevent.
    TEST_ASSERT_EQUAL_STRING("Phone Track|snap", r.snapshot().title.c_str());

    r.setMode(MediaRouter::Mode::Hu);
    TEST_ASSERT_EQUAL_STRING("HU Track", r.current().title.c_str());
    TEST_ASSERT_EQUAL_STRING("HU Track|snap", r.snapshot().title.c_str());
}

// The panel task's read goes through the source's own bounded copy too, and a
// busy source is reported as such, not papered over with current().
static void test_try_snapshot_is_forwarded_and_can_say_busy()
{
    FakeSource ams("ams", true);
    ams.setTitle("Phone Track");
    MediaRouter r;
    r.setSources(&ams, nullptr);
    r.setMode(MediaRouter::Mode::Ams);

    MediaInfo m;
    TEST_ASSERT_TRUE(r.trySnapshot(m, 5));
    TEST_ASSERT_EQUAL_STRING("Phone Track|snap", m.title.c_str());

    ams.setBusy(true);
    m.title = "kept";
    TEST_ASSERT_FALSE(r.trySnapshot(m, 5));
    TEST_ASSERT_EQUAL_STRING("kept", m.title.c_str());

    // No source in the slot: an empty answer, not a failure to retry for ever.
    r.setMode(MediaRouter::Mode::Hu);
    TEST_ASSERT_TRUE(r.trySnapshot(m, 5));
    TEST_ASSERT_TRUE(m.title.empty());
}

static void test_the_mode_is_readable_back()
{
    MediaRouter r;
    r.setMode(MediaRouter::Mode::Auto);
    TEST_ASSERT_EQUAL(MediaRouter::Mode::Auto, r.mode());
    TEST_ASSERT_EQUAL_STRING("auto", MediaRouter::modeStr(r.mode()));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_mode_strings_round_trip);
    RUN_TEST(test_junk_and_null_fall_back_to_ams);
    RUN_TEST(test_explicit_modes_pick_their_own_source_even_when_dead);
    RUN_TEST(test_auto_prefers_the_head_unit_while_it_is_live);
    RUN_TEST(test_auto_falls_back_to_the_phone_when_the_head_unit_goes_quiet);
    RUN_TEST(test_auto_with_no_head_unit_registered_is_just_the_phone);
    RUN_TEST(test_a_null_slot_is_answered_and_not_dereferenced);
    RUN_TEST(test_selecting_the_missing_half_of_a_pair_is_still_safe);
    RUN_TEST(test_the_router_forwards_rather_than_reimplements);
    RUN_TEST(test_the_mode_is_readable_back);
    RUN_TEST(test_try_snapshot_is_forwarded_and_can_say_busy);
    return UNITY_END();
}
