// Now playing from the head unit's app (AppMediaSource): what MegaOpen
// Companion posts, when the source counts as live, and the cover's handshake
// (the post names a hash, the board asks once, the bits must match it).
#include <unity.h>
#include <string.h>

#include "media/AppMediaSource.h"
#include "media/MediaRouter.h"

void setUp() {}
void tearDown() {}

namespace
{
    AppMediaSource::Update playing(const char* title, uint32_t art = 0)
    {
        AppMediaSource::Update u;
        u.app = "Spotify";
        u.title = title;
        u.artist = "Okean Elzy";
        u.album = "Supersymetriya";
        u.state = AppMediaSource::State::Playing;
        u.posMs = 30000;
        u.durMs = 200000;
        u.artHash = art;
        return u;
    }

    int g_changes = 0;
    void onChange(IMediaSource&, void*) { ++g_changes; }
}

// java.util.zip.CRC32 of "123456789" is the standard check value.
void test_crc32_is_javas()
{
    const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, AppMediaSource::crc32(s, sizeof(s)));
}

void test_a_post_fills_the_track_and_the_transport()
{
    AppMediaSource a;
    TEST_ASSERT_FALSE(a.active());
    TEST_ASSERT_EQUAL_STRING("NO APP", a.statusText());
    a.post(playing("Obijmy"), 1000);
    TEST_ASSERT_TRUE(a.active());
    const MediaInfo m = a.snapshot();
    TEST_ASSERT_EQUAL_STRING("Obijmy", m.title.c_str());
    TEST_ASSERT_EQUAL_STRING("Okean Elzy", m.artist.c_str());
    TEST_ASSERT_EQUAL_STRING("Spotify", m.playerName.c_str());
    TEST_ASSERT_TRUE(m.playing());
    TEST_ASSERT_EQUAL_FLOAT(30.0f, m.elapsedTime);
    TEST_ASSERT_EQUAL_FLOAT(200.0f, m.duration);
    // The position runs on by itself from the post's clock.
    TEST_ASSERT_EQUAL_FLOAT(40.0f, m.elapsedAt(11000));
    TEST_ASSERT_EQUAL_STRING("", a.statusText());
}

void test_paused_keeps_the_position_still()
{
    AppMediaSource a;
    AppMediaSource::Update u = playing("Obijmy");
    u.state = AppMediaSource::State::Paused;
    a.post(u, 1000);
    TEST_ASSERT_TRUE(a.active());
    const MediaInfo m = a.snapshot();
    TEST_ASSERT_FALSE(m.playing());
    TEST_ASSERT_EQUAL_FLOAT(30.0f, m.elapsedAt(60000));
}

void test_none_and_silence_end_the_source()
{
    AppMediaSource a;
    a.post(playing("Obijmy"), 1000);
    AppMediaSource::Update none;
    none.state = AppMediaSource::State::None;
    a.post(none, 2000);
    TEST_ASSERT_FALSE(a.active());
    TEST_ASSERT_EQUAL_STRING("APP: NOTHING PLAYS", a.statusText());

    a.post(playing("Obijmy"), 3000);
    a.setNow(3000 + AppMediaSource::LIVE_MS - 1);
    TEST_ASSERT_TRUE(a.active());
    a.setNow(3000 + AppMediaSource::LIVE_MS);
    TEST_ASSERT_FALSE(a.active());
    TEST_ASSERT_EQUAL_STRING("APP QUIET", a.statusText());
}

void test_only_a_change_notifies()
{
    AppMediaSource a;
    a.setChangeCallback(onChange, nullptr);
    g_changes = 0;
    a.post(playing("Obijmy"), 1000);
    TEST_ASSERT_EQUAL(1, g_changes);
    a.post(playing("Obijmy"), 21000);       // the keepalive: the same track
    TEST_ASSERT_EQUAL(1, g_changes);
    a.post(playing("Vidpusty"), 22000);
    TEST_ASSERT_EQUAL(2, g_changes);
}

void test_a_long_title_is_cut_on_a_character()
{
    AppMediaSource a;
    // 47 two-byte letters, then one more: 96 bytes would split the 49th.
    char t[200] = {0};
    for (int i = 0; i < 49; ++i) strcat(t, "\xD0\x96");   // Ж
    strcat(t, "x");
    a.post(playing(t), 1000);
    const MediaInfo m = a.snapshot();
    TEST_ASSERT_EQUAL(96, (int)m.title.size());
    TEST_ASSERT_EQUAL_HEX8(0xD0, (uint8_t)m.title[94]);
}

void test_the_cover_is_asked_for_once_and_must_match()
{
    uint8_t bits[AppMediaSource::ART_BYTES];
    for (int i = 0; i < AppMediaSource::ART_BYTES; ++i) bits[i] = (uint8_t)(i * 7);
    const uint32_t h = AppMediaSource::crc32(bits, sizeof(bits));

    AppMediaSource a;
    TEST_ASSERT_FALSE(a.post(playing("Obijmy"), 1000));      // no cover: nothing asked
    TEST_ASSERT_TRUE(a.post(playing("Obijmy", h), 2000));    // a cover named: asked
    TEST_ASSERT_EQUAL_HEX32(0, a.artHash());                 // not here yet
    uint8_t out[AppMediaSource::ART_BYTES];
    TEST_ASSERT_FALSE(a.art(out));

    uint8_t wrong[AppMediaSource::ART_BYTES];
    memcpy(wrong, bits, sizeof(wrong));
    wrong[5] ^= 1;
    TEST_ASSERT_FALSE(a.putArt(h, wrong));                   // the bits are not `h`
    TEST_ASSERT_FALSE(a.putArt(h ^ 1, bits));                // `h` is not what was named
    TEST_ASSERT_TRUE(a.putArt(h, bits));
    TEST_ASSERT_EQUAL_HEX32(h, a.artHash());
    TEST_ASSERT_TRUE(a.art(out));
    TEST_ASSERT_EQUAL_MEMORY(bits, out, sizeof(bits));
    TEST_ASSERT_FALSE(a.post(playing("Obijmy", h), 3000));   // here already: not asked again

    TEST_ASSERT_FALSE(a.post(playing("Vidpusty"), 4000));    // a track with none
    TEST_ASSERT_EQUAL_HEX32(0, a.artHash());
    TEST_ASSERT_FALSE(a.art(out));
}

// `auto`: the app while something plays on the head unit, then the head
// unit's own text, then the phone.
class Fake : public IMediaSource {
public:
    explicit Fake(const char* n) : _n(n) {}
    const MediaInfo& current() const override { return _i; }
    bool active() const override { return live; }
    const char* name() const override { return _n; }
    bool live = false;
private:
    MediaInfo _i;
    const char* _n;
};

void test_auto_puts_the_app_first()
{
    AppMediaSource app;
    Fake hu("hu"), ams("ams");
    MediaRouter r;
    r.setSources(&ams, &hu);
    r.setApp(&app);
    r.setMode(MediaRouter::Mode::Auto);
    hu.live = true;
    ams.live = true;
    TEST_ASSERT_EQUAL_STRING("hu", r.sourceName());
    app.post(playing("Obijmy"), 1000);
    TEST_ASSERT_EQUAL_STRING("app", r.sourceName());
    app.setNow(1000 + AppMediaSource::LIVE_MS);
    TEST_ASSERT_EQUAL_STRING("hu", r.sourceName());
}

void test_the_app_mode_is_a_setting()
{
    TEST_ASSERT_EQUAL((int)MediaRouter::Mode::App, (int)MediaRouter::modeFromStr("app"));
    TEST_ASSERT_EQUAL_STRING("app", MediaRouter::modeStr(MediaRouter::Mode::App));
    MediaRouter r;
    r.setMode(MediaRouter::Mode::App);
    TEST_ASSERT_EQUAL_STRING("none", r.sourceName());        // no slot yet: answered, not dereferenced
    AppMediaSource app;
    r.setApp(&app);
    TEST_ASSERT_EQUAL_STRING("app", r.sourceName());
    TEST_ASSERT_FALSE(r.active());
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc32_is_javas);
    RUN_TEST(test_a_post_fills_the_track_and_the_transport);
    RUN_TEST(test_paused_keeps_the_position_still);
    RUN_TEST(test_none_and_silence_end_the_source);
    RUN_TEST(test_only_a_change_notifies);
    RUN_TEST(test_a_long_title_is_cut_on_a_character);
    RUN_TEST(test_the_cover_is_asked_for_once_and_must_match);
    RUN_TEST(test_auto_puts_the_app_first);
    RUN_TEST(test_the_app_mode_is_a_setting);
    return UNITY_END();
}
