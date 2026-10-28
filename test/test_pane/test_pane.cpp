#include <unity.h>
#include <string.h>
// The `for (MediaPane* p : {&a, &b})` pairs below are brace-init lists, and
// deducing one needs this header named explicitly rather than dragged in by
// something else.
#include <initializer_list>
#include "display/MediaPane.h"
#include "display/PaneImages.h"

using MegaOpen::MediaPane;

void setUp() {}
void tearDown() {}

// The pane is 288 bytes of picture with no text in it, so "does it look right"
// is not a question a host test can answer and these do not pretend to. What
// they DO answer is everything that would corrupt the transfer or the panel
// rather than merely look wrong: writes outside the buffer, a scene table that
// has drifted from its enum, a divide by a zeroed star, an image that keeps
// changing after the music stopped, and a progress bar fed a position past the
// end of its track.
//
// That split is deliberate. A wrong pixel is a bench question; a write past 288
// bytes is a reboot, and a picture that never settles is 44 CAN frames a second
// on a bus this project has already put into BUS_OFF once.

// ---- the canary --------------------------------------------------------
// compose() is handed the middle of a larger buffer, and the margins either
// side must come back untouched. px() is the only thing that writes pixels and
// it clips — this is what proves it, for every scene, rather than by reading it.
static constexpr uint16_t MARGIN = 64;

static void composeGuarded(MediaPane& p, uint8_t* out)
{
    uint8_t buf[MARGIN + MediaPane::BYTES + MARGIN];
    memset(buf, 0xA5, sizeof(buf));
    p.compose(buf + MARGIN);
    for (uint16_t i = 0; i < MARGIN; ++i) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xA5, buf[i], "wrote before the pane");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(
            0xA5, buf[MARGIN + MediaPane::BYTES + i], "wrote past the pane");
    }
    if (out) memcpy(out, buf + MARGIN, MediaPane::BYTES);
}

// Compare ONE BAND of the pane. The media scene stacks four independent bands
// into 48 rows, and a change confined to one of them is not a change to the
// others — comparing whole panes conflates "the progress bar overran" with "the
// elapsed time reads differently", which are separate claims with separate
// answers. Rows are inclusive.
static int cmpRows(const uint8_t* a, const uint8_t* b, int y0, int y1)
{
    return memcmp(a + y0 * MediaPane::STRIDE, b + y0 * MediaPane::STRIDE,
                  (size_t)(y1 - y0 + 1) * MediaPane::STRIDE);
}
static constexpr int kTimesY0 = 0,  kTimesY1 = 6;    // elapsed left, remaining right
static constexpr int kBarY0   = 33, kBarY1   = 36;   // the progress bar

static uint16_t litPixels(const uint8_t* b)
{
    uint16_t n = 0;
    for (uint16_t i = 0; i < MediaPane::BYTES; ++i)
        for (uint8_t bit = 0; bit < 8; ++bit)
            if (b[i] & (1u << bit)) ++n;
    return n;
}

// ---- geometry ----------------------------------------------------------
static void test_the_buffer_is_the_size_the_library_declares()
{
    // 288 is not ours to choose: it is 6 bytes per row times 48 rows, and
    // AffaDisplay's kNavBitmapBytes and the OEM's own 302-byte 0x1F1 transfer
    // both depend on it. A mismatch here is a malformed message, not a bad
    // picture.
    TEST_ASSERT_EQUAL_UINT16(288, MediaPane::BYTES);
    TEST_ASSERT_EQUAL_UINT16(MediaPane::STRIDE * MediaPane::H, MediaPane::BYTES);
    TEST_ASSERT_EQUAL_UINT16(6, MediaPane::STRIDE);
}

static void test_the_top_left_pixel_is_bit_seven_of_byte_zero()
{
    // The OEM's packing, and the one thing that would render the whole pane
    // mirrored if it were wrong — a failure that looks like a drawing bug and
    // is a bit-order bug.
    uint8_t b[MediaPane::BYTES];
    memset(b, 0, sizeof(b));
    MediaPane::px(b, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(0x80, b[0]);

    memset(b, 0, sizeof(b));
    MediaPane::px(b, 7, 0);
    TEST_ASSERT_EQUAL_UINT8(0x01, b[0]);

    memset(b, 0, sizeof(b));
    MediaPane::px(b, 8, 0);
    TEST_ASSERT_EQUAL_UINT8(0x00, b[0]);
    TEST_ASSERT_EQUAL_UINT8(0x80, b[1]);

    memset(b, 0, sizeof(b));
    MediaPane::px(b, 0, 1);
    TEST_ASSERT_EQUAL_UINT8(0x80, b[MediaPane::STRIDE]);
}

static void test_pixels_outside_the_pane_are_dropped_not_wrapped()
{
    // A clipped pixel must vanish, NOT reappear on the next row. Wrapping would
    // be the quiet version of this bug: nothing crashes, and a progress bar that
    // overruns its track draws a stray dot in the times band.
    uint8_t b[MediaPane::BYTES];
    memset(b, 0, sizeof(b));
    MediaPane::px(b, 48, 0);
    MediaPane::px(b, -1, 0);
    MediaPane::px(b, 0, 48);
    MediaPane::px(b, 0, -1);
    MediaPane::px(b, 1000, 1000);
    for (uint16_t i = 0; i < MediaPane::BYTES; ++i)
        TEST_ASSERT_EQUAL_UINT8(0, b[i]);
}

// ---- the scene table ---------------------------------------------------
static void test_every_scene_round_trips_through_its_name()
{
    // Settings are stored BY NAME so that inserting a scene cannot repoint a
    // saved one at its neighbour. That only holds if the table and the enum
    // agree in both directions, for every member.
    for (uint8_t i = 0; i < MediaPane::SCENE_COUNT; ++i) {
        const auto s = (MediaPane::Scene)i;
        const char* n = MediaPane::sceneName(s);
        TEST_ASSERT_NOT_NULL(n);
        TEST_ASSERT_TRUE_MESSAGE(n[0] != '\0', "a scene with an empty name");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)MediaPane::sceneFromName(n), i,
                                        "scene name did not round-trip");
    }
}

static void test_an_unknown_or_null_scene_name_falls_back_to_off()
{
    // Off and not Media: an NVS key from an older build, or a typo in a POST,
    // must not silently switch on the most expensive thing this firmware puts
    // on the bus.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MediaPane::Scene::Off,
                            (uint8_t)MediaPane::sceneFromName("spectrum"));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MediaPane::Scene::Off,
                            (uint8_t)MediaPane::sceneFromName(""));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MediaPane::Scene::Off,
                            (uint8_t)MediaPane::sceneFromName(nullptr));
}

static void test_setting_a_scene_out_of_range_falls_back_to_off()
{
    MediaPane p;
    p.setScene((MediaPane::Scene)200);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MediaPane::Scene::Off, (uint8_t)p.scene());
    TEST_ASSERT_FALSE(p.on());
}

static void test_only_media_and_tape_claim_to_use_media()
{
    // The page says "оздоблення" on the strength of this. A scene that wrongly
    // claimed real data would be inviting somebody to read a level off a random
    // walk. Tape's packs are the real position.
    for (uint8_t i = 0; i < MediaPane::SCENE_COUNT; ++i) {
        const auto s = (MediaPane::Scene)i;
        TEST_ASSERT_EQUAL_MESSAGE(s == MediaPane::Scene::Media || s == MediaPane::Scene::Tape,
                                  MediaPane::usesMedia(s),
                                  "usesMedia disagrees with the real scenes");
    }
    TEST_ASSERT_FALSE(MediaPane::animates(MediaPane::Scene::Off));
}

// ---- every scene, safely ------------------------------------------------
static void test_no_scene_writes_outside_the_pane()
{
    // The whole gallery, stepped well past any wrap in its own state, with a
    // guard band either side. Bounce reverses, Stars respawn, Wave fills its
    // ring and Rings wraps its modulus inside 200 frames.
    for (uint8_t i = 0; i < MediaPane::SCENE_COUNT; ++i) {
        MediaPane p;
        p.setScene((MediaPane::Scene)i);
        p.setProgress(61, 213);
        p.setPlaying(true);
        p.setClockSecs(23 * 3600 + 59 * 60 + 59);
        for (int f = 0; f < 200; ++f) {
            p.step();
            composeGuarded(p, nullptr);
        }
    }
}

static void test_a_pane_composed_before_it_was_ever_stepped_is_safe()
{
    // The web page composes a preview copy, and it may do so before the loop
    // task has stepped the pane even once. compose() is const and cannot seed,
    // so the zero-initialised starfield has to survive its own perspective
    // divide rather than divide by zero.
    for (uint8_t i = 0; i < MediaPane::SCENE_COUNT; ++i) {
        MediaPane p;
        p.setScene((MediaPane::Scene)i);
        composeGuarded(p, nullptr);
    }
}

static void test_off_composes_an_empty_pane()
{
    // The only "remove the layer" this panel has. If Off drew anything, turning
    // the pane off would leave the last picture on the glass for ever.
    MediaPane p;
    p.setScene(MediaPane::Scene::Media);
    p.setPlaying(true);
    for (int i = 0; i < 20; ++i) p.step();

    p.setScene(MediaPane::Scene::Off);
    uint8_t b[MediaPane::BYTES];
    composeGuarded(p, b);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, litPixels(b), "Scene::Off drew something");
}

// ---- the media scene ----------------------------------------------------
static void test_a_paused_track_settles_and_then_stops_changing()
{
    // THE ONE THAT PAYS FOR ITSELF. The caller stops sending when two composed
    // frames are identical, so a spectrum that never settles is 44 CAN frames
    // per transfer for ever over a track nobody is playing.
    MediaPane p;
    p.setScene(MediaPane::Scene::Media);
    p.setPlaying(false);
    p.setProgress(30, 200);

    uint8_t a[MediaPane::BYTES], b[MediaPane::BYTES];
    for (int i = 0; i < 200; ++i) p.step();      // long enough for every peak to decay
    p.compose(a);
    p.step();
    p.compose(b);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(a, b, MediaPane::BYTES),
                                  "a paused pane never settles");
}

static void test_a_playing_track_keeps_changing()
{
    // The other half of the same contract: if it settled while playing, the
    // pane would freeze mid-song and look like a crashed link.
    MediaPane p;
    p.setScene(MediaPane::Scene::Media);
    p.setPlaying(true);
    p.setProgress(30, 200);

    uint8_t a[MediaPane::BYTES], b[MediaPane::BYTES];
    for (int i = 0; i < 20; ++i) p.step();
    p.compose(a);

    bool moved = false;
    for (int i = 0; i < 10 && !moved; ++i) {
        p.step();
        p.compose(b);
        moved = memcmp(a, b, MediaPane::BYTES) != 0;
    }
    TEST_ASSERT_TRUE_MESSAGE(moved, "a playing spectrum stopped moving");
}

static void test_a_source_with_no_transport_drops_the_bands_it_cannot_fill()
{
    // The head unit reports no position and no play state. A bar frozen at zero
    // is not missing information but WRONG information — the same rule that
    // strips row 1 of the text screen. Fewer lit pixels is the observable form:
    // the times, the progress bar and the transport glyph are all gone.
    MediaPane withT, withoutT;
    for (MediaPane* p : {&withT, &withoutT}) {
        p->setScene(MediaPane::Scene::Media);
        p->setProgress(61, 213);
        p->setPlaying(true);
    }
    withoutT.setHasTransport(false);

    uint8_t a[MediaPane::BYTES], b[MediaPane::BYTES];
    withT.compose(a);
    withoutT.compose(b);
    TEST_ASSERT_TRUE_MESSAGE(litPixels(b) < litPixels(a),
                             "a transportless pane drew as much as a full one");
}

static void test_a_position_past_the_end_of_the_track_cannot_overrun_the_bar()
{
    // AMS really does report an elapsed past the duration across a seek. The
    // fill is clamped, so the bar tops out rather than drawing off the end of
    // its box — which would look like a stray line through the transport glyph.
    //
    // THE BAR BAND ONLY. The times band legitimately differs between these two
    // and comparing whole panes would be asserting that it does not: an elapsed
    // of 60000 s really is a different number from 200, and drawing it as such
    // is right. Only the bar is claimed to saturate.
    MediaPane a, b;
    for (MediaPane* p : {&a, &b}) {
        p->setScene(MediaPane::Scene::Media);
        p->setPlaying(true);
    }
    a.setProgress(200, 200);
    b.setProgress(60000, 200);

    uint8_t ba[MediaPane::BYTES], bb[MediaPane::BYTES];
    composeGuarded(a, ba);
    composeGuarded(b, bb);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmpRows(ba, bb, kBarY0, kBarY1),
                                  "an overrun position drew a different bar");

    // AND THE BAR MUST ACTUALLY TRACK THE POSITION, which the equality above
    // does not prove on its own — px() clips, so a bar drawn far too long lights
    // the same pixels as a full one and the assertion passes with the clamp
    // deleted. A half-full bar differing from a full one is what makes the pair
    // evidence rather than a tautology.
    MediaPane half;
    half.setScene(MediaPane::Scene::Media);
    half.setPlaying(true);
    half.setProgress(100, 200);
    uint8_t bh[MediaPane::BYTES];
    composeGuarded(half, bh);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, cmpRows(bh, ba, kBarY0, kBarY1),
                                  "the bar does not track the position at all");
}

static void test_a_zero_duration_draws_an_empty_bar_rather_than_dividing_by_it()
{
    // "No duration known" is the ordinary state for the first seconds of a
    // track, and for every track a live stream reports.
    MediaPane p;
    p.setScene(MediaPane::Scene::Media);
    p.setProgress(45, 0);
    p.setPlaying(true);
    composeGuarded(p, nullptr);
}

static void test_a_track_longer_than_the_field_saturates_rather_than_wrapping()
{
    // 100 minutes drawn as 40:00 is a plausible-looking wrong number, which is
    // the worst kind on a dashboard: nothing about it says it is wrong.
    MediaPane a, b;
    for (MediaPane* p : {&a, &b}) {
        p->setScene(MediaPane::Scene::Media);
        p->setPlaying(false);
    }
    //
    // DURATION ZERO, so that ELAPSED is the only thing that differs between the
    // two. With a duration set, the remaining time and the bar fill would differ
    // — legitimately, because these really are different points in a track — and
    // the assertion would be about those rather than about the saturation. The
    // no-duration case is also the honest one: an elapsed of 65000 s arrives
    // from a live stream, which is exactly where no duration is reported.
    a.setProgress(99 * 60 + 59, 0);   // 5999 — the largest the field can show
    b.setProgress(65000, 0);          // must be drawn as the same 99:59

    uint8_t ba[MediaPane::BYTES], bb[MediaPane::BYTES];
    a.compose(ba);
    b.compose(bb);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(ba, bb, MediaPane::BYTES),
                                  "an over-long position did not saturate");
}

// ---- determinism --------------------------------------------------------
static void test_two_panes_stepped_alike_draw_alike()
{
    // The animation is a seeded xorshift, deliberately, so that a frame can be
    // reproduced when somebody photographs the glass and asks what happened.
    // Nothing here may reach for a real clock or a hardware RNG.
    MediaPane a, b;
    for (MediaPane* p : {&a, &b}) {
        p->setScene(MediaPane::Scene::Media);
        p->setPlaying(true);
        p->setProgress(12, 180);
        for (int i = 0; i < 137; ++i) p->step();
    }
    uint8_t ba[MediaPane::BYTES], bb[MediaPane::BYTES];
    a.compose(ba);
    b.compose(bb);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(ba, bb, MediaPane::BYTES),
                                  "the pane is not deterministic");
}

static void test_the_carousel_visits_every_scene_but_off_and_custom_and_wraps()
{
    // Each stop once, never Off, GMeter, the pictures or Custom, and back to
    // the start — the ui task trusts this for the ring and only supplies the clock.
    constexpr uint8_t STOPS = (uint8_t)MediaPane::Scene::GMeter - 1;   // everything before GMeter but Off
    bool seen[MediaPane::SCENE_COUNT] = {false};
    MediaPane::Scene s = MediaPane::Scene::Media;
    for (uint8_t i = 0; i < STOPS; ++i) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE((uint8_t)MediaPane::Scene::Off, (uint8_t)s,
                                      "the carousel stopped on Off");
        TEST_ASSERT_NOT_EQUAL_MESSAGE((uint8_t)MediaPane::Scene::Custom, (uint8_t)s,
                                      "the carousel stopped on Custom");
        TEST_ASSERT_NOT_EQUAL_MESSAGE((uint8_t)MediaPane::Scene::GMeter, (uint8_t)s,
                                      "the carousel stopped on GMeter");
        TEST_ASSERT_FALSE_MESSAGE(seen[(uint8_t)s], "a scene came round twice in one turn");
        seen[(uint8_t)s] = true;
        s = MediaPane::carouselNext(s);
    }
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MediaPane::Scene::Media, (uint8_t)s);
    for (uint8_t i = 1; i < (uint8_t)MediaPane::Scene::GMeter; ++i)
        TEST_ASSERT_TRUE_MESSAGE(seen[i], "a scene was never visited");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MediaPane::Scene::Media,
                            (uint8_t)MediaPane::carouselNext(MediaPane::Scene::GMeter));
    TEST_ASSERT_FALSE(seen[(uint8_t)MediaPane::Scene::Off]);
    // From Off — a pane switched on into "auto" — the first stop is Media; from
    // Custom too: "auto" chosen while the picture shows keeps it until the
    // next carousel stop, which is Media.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MediaPane::Scene::Media,
                            (uint8_t)MediaPane::carouselNext(MediaPane::Scene::Off));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MediaPane::Scene::Media,
                            (uint8_t)MediaPane::carouselNext(MediaPane::Scene::Custom));
}

// ---- the custom picture (POST /api/media bits=) ---------------------------
static void test_the_custom_scene_draws_the_stored_picture_and_holds_still()
{
    // Byte for byte what was set, through the same compose the wire uses, and
    // the same after any number of steps: a still picture is sent once.
    uint8_t pic[MediaPane::BYTES];
    for (uint16_t i = 0; i < MediaPane::BYTES; ++i) pic[i] = (uint8_t)(i * 37 + 5);
    MediaPane p;
    p.setScene(MediaPane::Scene::Custom);
    TEST_ASSERT_FALSE(MediaPane::animates(MediaPane::Scene::Custom));
    uint8_t b[MediaPane::BYTES];
    composeGuarded(p, b);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, litPixels(b), "an unset picture is not dark");
    p.setCustom(pic);
    for (int f = 0; f < 10; ++f) p.step();
    composeGuarded(p, b);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(pic, b, MediaPane::BYTES), "not the stored picture");
    // A borrow, not a copy: the ui task re-points it after each apply, and a
    // change in the lent buffer shows on the next compose.
    pic[0] ^= 0xFF;
    composeGuarded(p, b);
    TEST_ASSERT_EQUAL_HEX8(pic[0], b[0]);
    // Nothing lent: dark, not a null read.
    p.setCustom(nullptr);
    composeGuarded(p, b);
    TEST_ASSERT_EQUAL_UINT16(0, litPixels(b));
}

static void test_the_picture_survives_a_round_trip_through_hex()
{
    uint8_t pic[MediaPane::BYTES], back[MediaPane::BYTES];
    for (uint16_t i = 0; i < MediaPane::BYTES; ++i) pic[i] = (uint8_t)(255 - i);
    char hex[MediaPane::BYTES * 2 + 1];
    MediaPane::toHex(pic, hex);
    TEST_ASSERT_EQUAL_UINT32(MediaPane::BYTES * 2, strlen(hex));
    TEST_ASSERT_EQUAL_STRING_LEN("fffefdfc", hex, 8);
    TEST_ASSERT_TRUE(MediaPane::fromHex(hex, back));
    TEST_ASSERT_EQUAL_INT(0, memcmp(pic, back, MediaPane::BYTES));
    // Upper case is accepted; the page sends lower.
    for (char* c = hex; *c; ++c) if (*c >= 'a' && *c <= 'f') *c = (char)(*c - 32);
    memset(back, 0, sizeof(back));
    TEST_ASSERT_TRUE(MediaPane::fromHex(hex, back));
    TEST_ASSERT_EQUAL_INT(0, memcmp(pic, back, MediaPane::BYTES));
    // MSB is the left pixel: "80" then zeros lights (0,0) only.
    char one[MediaPane::BYTES * 2 + 1];
    memset(one, '0', MediaPane::BYTES * 2);
    one[MediaPane::BYTES * 2] = '\0';
    one[0] = '8';
    TEST_ASSERT_TRUE(MediaPane::fromHex(one, back));
    TEST_ASSERT_EQUAL_HEX8(0x80, back[0]);
}

static void test_a_bad_hex_picture_is_refused_whole()
{
    // Refused without touching the output: a half-written picture would be
    // staged and shown.
    char hex[MediaPane::BYTES * 2 + 2];
    memset(hex, 'a', MediaPane::BYTES * 2);
    hex[MediaPane::BYTES * 2] = '\0';
    uint8_t out[MediaPane::BYTES];
    TEST_ASSERT_TRUE(MediaPane::fromHex(hex, nullptr));          // validate only
    memset(out, 0x11, sizeof(out));
    hex[MediaPane::BYTES * 2 - 1] = 'g';                         // one bad digit, at the end
    TEST_ASSERT_FALSE(MediaPane::fromHex(hex, out));
    TEST_ASSERT_EQUAL_HEX8(0x11, out[0]);
    hex[MediaPane::BYTES * 2 - 1] = 'a';
    hex[MediaPane::BYTES * 2] = 'a'; hex[MediaPane::BYTES * 2 + 1] = '\0';   // one too many
    TEST_ASSERT_FALSE(MediaPane::fromHex(hex, out));
    hex[MediaPane::BYTES * 2 - 1] = '\0';                       // one too few
    TEST_ASSERT_FALSE(MediaPane::fromHex(hex, out));
    TEST_ASSERT_FALSE(MediaPane::fromHex("", out));
    TEST_ASSERT_FALSE(MediaPane::fromHex(nullptr, out));
    TEST_ASSERT_EQUAL_HEX8(0x11, out[0]);
}

static void test_every_decorative_scene_moves_when_stepped()
{
    // "Animates" is a promise the carousel relies on: a stop that drew the same
    // 288 bytes for thirty seconds would look like the link had died. Clock is
    // excluded because it moves with the time it is given, not with step(), and
    // Media is covered by its own playing/paused tests.
    for (uint8_t i = 0; i < MediaPane::SCENE_COUNT; ++i) {
        const auto s = (MediaPane::Scene)i;
        if (s == MediaPane::Scene::Off || s == MediaPane::Scene::Media ||
            s == MediaPane::Scene::Tape || s == MediaPane::Scene::GMeter ||
            s == MediaPane::Scene::Clock || s == MediaPane::Scene::Custom ||
            s == MediaPane::Scene::Tryzub || s == MediaPane::Scene::TryzubClock ||
            s == MediaPane::Scene::Renault) continue;
        MediaPane p;
        p.setScene(s);
        p.step();
        uint8_t a[MediaPane::BYTES], b[MediaPane::BYTES];
        composeGuarded(p, a);
        TEST_ASSERT_TRUE_MESSAGE(litPixels(a) > 0, "a decorative scene drew nothing");
        for (int f = 0; f < 3; ++f) p.step();
        composeGuarded(p, b);
        TEST_ASSERT_TRUE_MESSAGE(memcmp(a, b, MediaPane::BYTES) != 0,
                                 MediaPane::sceneName(s));
    }
}

// ---- the pictures (PaneImages.h) ---------------------------------------
static void test_pictures_draw_and_the_tryzub_clock_tells_the_time()
{
    MediaPane p;
    uint8_t a[MediaPane::BYTES], b[MediaPane::BYTES];
    p.setScene(MediaPane::Scene::Tryzub);
    composeGuarded(p, a);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(MegaOpen::PaneImages::kTryzub, a, MediaPane::BYTES);
    TEST_ASSERT_FALSE(MediaPane::animates(MediaPane::Scene::Tryzub));
    p.setScene(MediaPane::Scene::Renault);
    composeGuarded(p, a);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(MegaOpen::PaneImages::kRenault, a, MediaPane::BYTES);
    // The small tryzub on top, the time under it; another minute, another image.
    p.setScene(MediaPane::Scene::TryzubClock);
    p.setClockSecs(10 * 3600 + 23 * 60);
    composeGuarded(p, a);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(MegaOpen::PaneImages::kTryzubSmall, a, sizeof(MegaOpen::PaneImages::kTryzubSmall));
    bool under = false;   // the digits, in rows 31..47
    for (size_t k = sizeof(MegaOpen::PaneImages::kTryzubSmall); k < MediaPane::BYTES; ++k) under |= a[k] != 0;
    TEST_ASSERT_TRUE(under);
    p.setClockSecs(10 * 3600 + 24 * 60);
    composeGuarded(p, b);
    TEST_ASSERT_TRUE(memcmp(a, b, MediaPane::BYTES) != 0);
    // Not carousel stops: a picture is picked on purpose.
    for (uint8_t i = 1; i < MediaPane::SCENE_COUNT; ++i) {
        const auto n = MediaPane::carouselNext((MediaPane::Scene)i);
        TEST_ASSERT_TRUE(n != MediaPane::Scene::Tryzub && n != MediaPane::Scene::TryzubClock &&
                         n != MediaPane::Scene::Renault);
    }
}

// ---- the G-meter (0x2A0) ------------------------------------------------
static bool on(const uint8_t* b, int x, int y)
{
    return b[y * MediaPane::STRIDE + (x >> 3)] & (0x80 >> (x & 7));
}

static void test_a_stale_gmeter_is_a_still_dotted_ring()
{
    // Stale: a dotted 0.5 g ring and the centre, and the same
    // bytes however long it runs or whatever values arrive. Never motion.
    MediaPane p;
    p.setScene(MediaPane::Scene::GMeter);
    uint8_t a[MediaPane::BYTES], b[MediaPane::BYTES];
    composeGuarded(p, a);
    TEST_ASSERT_TRUE(on(a, 23, 23));
    TEST_ASSERT_TRUE(on(a, 33, 23));            // 0 degrees, r 11 (isin tops at 1016): lit
    TEST_ASSERT_FALSE(on(a, 45, 23));           // no 1 g tick
    const uint16_t dim = litPixels(a);
    for (int i = 0; i < 10; ++i) {
        p.setAccel((int16_t)(i * 90), (int16_t)(-i * 70), false, false, (uint32_t)i * 500);
        p.step();
    }
    composeGuarded(p, b);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(a, b, MediaPane::BYTES), "a stale G-meter moved");
    // Dimmer than the live ring: about half its points.
    p.setAccel(0, 0, true, true, 0);
    composeGuarded(p, b);
    TEST_ASSERT_TRUE(dim < litPixels(b));
}

static void test_the_gmeter_draws_the_dot_where_the_pull_is()
{
    MediaPane p;
    p.setScene(MediaPane::Scene::GMeter);
    uint8_t b[MediaPane::BYTES];
    // At rest: the 3x3 dot on the centre, the solid ring, the 1 g ticks.
    p.setAccel(0, 0, true, true, 0);
    composeGuarded(p, b);
    for (int y = 22; y <= 24; ++y)
        for (int x = 22; x <= 24; ++x) TEST_ASSERT_TRUE(on(b, x, y));
    TEST_ASSERT_TRUE(on(b, 33, 23));
    TEST_ASSERT_TRUE(on(b, 13, 23));
    TEST_ASSERT_TRUE(on(b, 1, 23));
    TEST_ASSERT_TRUE(on(b, 45, 23));
    TEST_ASSERT_TRUE(on(b, 23, 1));
    TEST_ASSERT_TRUE(on(b, 23, 45));
    // 0.5 g to the right: the dot sits on the ring, 11 px right.
    MediaPane q;
    q.setScene(MediaPane::Scene::GMeter);
    q.setAccel(500, 0, true, true, 0);
    composeGuarded(q, b);
    for (int y = 22; y <= 24; ++y)
        for (int x = 33; x <= 35; ++x) TEST_ASSERT_TRUE(on(b, x, y));
    TEST_ASSERT_FALSE(on(b, 22, 22));           // the centre is only its pixel now
    // Braking at 1 g (felt: thrown forward) draws up, to the top tick; past
    // 1 g clamps there. Speeding up draws down.
    MediaPane r;
    r.setScene(MediaPane::Scene::GMeter);
    r.setAccel(0, -2000, true, true, 0);
    composeGuarded(r, b);
    TEST_ASSERT_TRUE(on(b, 23, 1));
    TEST_ASSERT_TRUE(on(b, 22, 2));
    TEST_ASSERT_TRUE(on(b, 24, 0));
    MediaPane d;
    d.setScene(MediaPane::Scene::GMeter);
    d.setAccel(0, 500, true, true, 0);
    composeGuarded(d, b);
    TEST_ASSERT_TRUE(on(b, 23, 34) && on(b, 22, 33) && on(b, 24, 35));
}

static void test_an_unverified_gmeter_moves_with_a_dotted_ring()
{
    // Real bus data, decode not yet confirmed: the owner has to see it move
    // to confirm it, so the dot moves; the ring is dotted, the "?" mark.
    MediaPane u, v;
    u.setScene(MediaPane::Scene::GMeter);
    v.setScene(MediaPane::Scene::GMeter);
    uint8_t a[MediaPane::BYTES], b[MediaPane::BYTES];
    u.setAccel(500, 0, true, false, 0);
    v.setAccel(500, 0, true, true, 0);
    composeGuarded(u, a);
    composeGuarded(v, b);
    for (int y = 22; y <= 24; ++y)
        for (int x = 33; x <= 35; ++x) TEST_ASSERT_TRUE(on(a, x, y));
    TEST_ASSERT_TRUE(on(a, 45, 23));                    // the 1 g ticks: it is live
    TEST_ASSERT_TRUE_MESSAGE(litPixels(a) < litPixels(b), "the unverified ring is not dotted");
    TEST_ASSERT_TRUE(on(b, 13, 23) && on(b, 23, 13));   // solid where it is verified
    // And it moves.
    u.setAccel(-500, 0, true, false, 500);
    composeGuarded(u, b);
    TEST_ASSERT_TRUE(on(b, 12, 23));
    TEST_ASSERT_TRUE(memcmp(a, b, MediaPane::BYTES) != 0);
}

static void test_the_gmeter_holds_the_peak_for_three_seconds()
{
    MediaPane p;
    p.setScene(MediaPane::Scene::GMeter);
    uint8_t b[MediaPane::BYTES];
    p.setAccel(1000, 0, true, true, 0);             // the peak: 1 g right, x = 45
    p.setAccel(0, 0, true, true, 1000);
    p.setAccel(0, 0, true, true, 2000);
    p.setAccel(0, 0, true, true, 2900);
    composeGuarded(p, b);
    TEST_ASSERT_TRUE_MESSAGE(on(b, 44, 22) && on(b, 46, 22) && on(b, 44, 24) && on(b, 46, 24),
                             "no peak x at 2.9 s");
    // A weaker pull does not replace it; 3 s after it, the peak lets go.
    p.setAccel(300, 0, true, true, 3000);
    composeGuarded(p, b);
    TEST_ASSERT_FALSE(on(b, 44, 22));
    TEST_ASSERT_FALSE(on(b, 46, 24));
}

static void test_the_gmeter_drops_its_trail_when_the_data_goes()
{
    MediaPane p, fresh;
    p.setScene(MediaPane::Scene::GMeter);
    fresh.setScene(MediaPane::Scene::GMeter);
    for (int i = 0; i < 6; ++i) p.setAccel((int16_t)(i * 150), (int16_t)(i * 100), true, true, (uint32_t)i * 500);
    p.setAccel(0, 0, false, false, 3000);
    uint8_t a[MediaPane::BYTES], b[MediaPane::BYTES];
    composeGuarded(p, a);
    composeGuarded(fresh, b);
    TEST_ASSERT_EQUAL_INT(0, memcmp(a, b, MediaPane::BYTES));
    // Back: one point, no old trail and no old peak.
    p.setAccel(0, 0, true, true, 3500);
    fresh.setAccel(0, 0, true, true, 3500);
    composeGuarded(p, a);
    composeGuarded(fresh, b);
    TEST_ASSERT_EQUAL_INT(0, memcmp(a, b, MediaPane::BYTES));
}

static void test_accel_long_converts_to_milli_g()
{
    TEST_ASSERT_EQUAL_INT32(1000, MediaPane::mgFromMs2x100(981));
    TEST_ASSERT_EQUAL_INT32(-499, MediaPane::mgFromMs2x100(-490));
    TEST_ASSERT_EQUAL_INT32(0, MediaPane::mgFromMs2x100(0));
    TEST_ASSERT_EQUAL_INT32(2803, MediaPane::mgFromMs2x100(2750));   // the matrix's top
}

// Tape: the reels turn while playing and stop when paused, like a deck.
static void test_the_tape_turns_only_while_playing()
{
    MediaPane p;
    p.setScene(MediaPane::Scene::Tape);
    p.setProgress(60, 200);
    p.setPlaying(true);
    uint8_t a[MediaPane::BYTES], b[MediaPane::BYTES];
    p.step();
    composeGuarded(p, a);
    TEST_ASSERT_TRUE(litPixels(a) > 0);
    p.step(); p.step();
    composeGuarded(p, b);
    TEST_ASSERT_TRUE_MESSAGE(memcmp(a, b, MediaPane::BYTES) != 0, "playing, the reels stood still");
    p.setPlaying(false);
    composeGuarded(p, a);
    for (int f = 0; f < 5; ++f) p.step();
    composeGuarded(p, b);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(a, b, MediaPane::BYTES), "paused, the reels turned");
    // The head unit has no play state: turning, as Media's bars run.
    p.setHasTransport(false);
    for (int f = 0; f < 3; ++f) p.step();
    composeGuarded(p, a);
    TEST_ASSERT_TRUE(memcmp(a, b, MediaPane::BYTES) != 0);
}

// Lit pixels in a column band: the left reel lives in x 2..28, the right in
// 19..45; their outer quarters are each reel's alone.
static uint16_t litIn(const uint8_t* b, int x0, int x1)
{
    uint16_t n = 0;
    for (int y = 7; y < 34; ++y)
        for (int x = x0; x <= x1; ++x)
            if (b[y * MediaPane::STRIDE + (x >> 3)] & (0x80 >> (x & 7))) ++n;
    return n;
}

// The tape winds from left to right with the position: at the start the
// left pack is the big one, at the end the right.
static void test_the_tape_winds_with_the_position()
{
    MediaPane p;
    p.setScene(MediaPane::Scene::Tape);
    uint8_t a[MediaPane::BYTES], b[MediaPane::BYTES];
    p.setProgress(0, 200);
    composeGuarded(p, a);
    p.setProgress(200, 200);
    composeGuarded(p, b);
    TEST_ASSERT_TRUE(litIn(a, 0, 8) > litIn(b, 0, 8));
    TEST_ASSERT_TRUE(litIn(b, 39, 47) > litIn(a, 39, 47));
    // Past the end (a seek, AMS rounding) is the end, not beyond it.
    p.setProgress(500, 200);
    uint8_t c[MediaPane::BYTES];
    composeGuarded(p, c);
    TEST_ASSERT_EQUAL_INT(0, memcmp(b, c, MediaPane::BYTES));
}

static void test_life_reseeds_rather_than_freezing()
{
    // A 48x48 torus settles into blinkers and still lifes; the scene reseeds
    // itself after kLifeMaxAge frames or when the population collapses. So no
    // run of identical frames may outlast that age — and the board never
    // empties, because an empty board would be a pane that looks Off.
    MediaPane p;
    p.setScene(MediaPane::Scene::Life);
    uint8_t prev[MediaPane::BYTES], cur[MediaPane::BYTES];
    p.step(); composeGuarded(p, prev);
    int same = 0, longest = 0;
    for (int f = 0; f < 600; ++f) {
        p.step();
        composeGuarded(p, cur);
        TEST_ASSERT_TRUE_MESSAGE(litPixels(cur) > 0, "Life went dark");
        if (memcmp(cur, prev, MediaPane::BYTES) == 0) { if (++same > longest) longest = same; }
        else same = 0;
        memcpy(prev, cur, MediaPane::BYTES);
    }
    TEST_ASSERT_TRUE_MESSAGE(longest < 170, "Life froze for longer than its reseed age");
}

static void test_compose_does_not_advance_the_animation()
{
    // The web preview composes a copy every refresh. If compose() stepped
    // anything, an open browser would run the pane faster than the panel sees
    // it — and the preview would then be showing a frame the glass never got.
    MediaPane p;
    p.setScene(MediaPane::Scene::Media);
    p.setPlaying(true);
    for (int i = 0; i < 10; ++i) p.step();

    uint8_t a[MediaPane::BYTES], b[MediaPane::BYTES];
    p.compose(a);
    for (int i = 0; i < 5; ++i) p.compose(b);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(a, b, MediaPane::BYTES),
                                  "compose() mutated the pane");
}

static void test_the_clock_face_reads_the_time_it_was_given()
{
    // The one scene whose whole point is being right. Two different times must
    // not draw the same face — which is what a clock wired to the wrong field,
    // or to nothing, would do.
    MediaPane a, b;
    for (MediaPane* p : {&a, &b}) p->setScene(MediaPane::Scene::Clock);
    a.setClockSecs(10 * 3600 + 8 * 60 + 30);
    b.setClockSecs(2 * 3600 + 44 * 60 + 5);

    uint8_t ba[MediaPane::BYTES], bb[MediaPane::BYTES];
    composeGuarded(a, ba);
    composeGuarded(b, bb);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, memcmp(ba, bb, MediaPane::BYTES),
                                  "the clock face ignored the time");
}

static void test_the_clock_wraps_a_day_rather_than_running_off_the_face()
{
    // Seconds-since-midnight is fed from tm_hour/tm_min/tm_sec, but the setter
    // takes a plain uint32 and a caller could hand it anything.
    MediaPane a, b;
    for (MediaPane* p : {&a, &b}) p->setScene(MediaPane::Scene::Clock);
    a.setClockSecs(3661);
    b.setClockSecs(86400u + 3661u);

    uint8_t ba[MediaPane::BYTES], bb[MediaPane::BYTES];
    composeGuarded(a, ba);
    composeGuarded(b, bb);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(ba, bb, MediaPane::BYTES),
                                  "the clock did not wrap at a day");
}

// ---- the times band -----------------------------------------------------
static void test_a_two_digit_remaining_time_is_not_truncated()
{
    // THE BUG THIS WAS WRITTEN FOR. The remaining field was drawn at a hard
    // x = 26, which fits "M:SS" (21 columns) and not "MM:SS" (27): 26 + 27 = 53
    // on a 48-column pane, so px() clipped the seconds-units glyph and 12:34
    // reached the glass as 12:3. A wrong number, silently, on the one scene
    // whose purpose is real data — and every track with ten minutes left showed
    // it.
    //
    // The observable form: the last drawn column of the remaining field must be
    // inside the pane, so the rightmost lit pixel of the times band is < 48 AND
    // the field is not missing pixels that a same-width field at a fitting
    // position would have. Comparing against the same value drawn at a position
    // known to fit is what makes this independent of the layout constants.
    MediaPane p;
    p.setScene(MediaPane::Scene::Media);
    p.setPlaying(true);
    p.setProgress(61, 815);            // 12:34 remaining, two-digit minutes

    uint8_t got[MediaPane::BYTES];
    composeGuarded(p, got);

    uint8_t want[MediaPane::BYTES];
    memset(want, 0, sizeof(want));
    const uint8_t w = MediaPane::timeWidth(815 - 61);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(27, w, "MM:SS is not 27 columns wide");
    MediaPane::timeAt(want, (int)MediaPane::W - (int)w, 0, 815 - 61);

    // Every pixel of the reference remaining-field must be present in the pane.
    for (int y = kTimesY0; y <= kTimesY1; ++y)
        for (int i = 0; i < MediaPane::STRIDE; ++i) {
            const uint16_t k = (uint16_t)(y * MediaPane::STRIDE + i);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(want[k] & got[k], want[k],
                                            "the remaining time lost pixels");
        }
}

static void test_two_long_times_drop_the_second_rather_than_overlapping()
{
    // 27 + 27 needs 54 columns and the pane has 48, so the pair cannot both be
    // drawn. Overlapping them would produce an unreadable smear that still looks
    // like a number; omitting one leaves the elapsed time correct. Missing
    // information beats wrong information — this project's own rule.
    MediaPane both, onlyElapsed;
    for (MediaPane* p : {&both, &onlyElapsed}) {
        p->setScene(MediaPane::Scene::Media);
        p->setPlaying(true);
    }
    both.setProgress(11 * 60, 40 * 60);          // 11:00 elapsed, 29:00 left
    onlyElapsed.setProgress(11 * 60, 11 * 60);   // 11:00 elapsed, 0:00 left

    uint8_t bb[MediaPane::BYTES], be[MediaPane::BYTES];
    composeGuarded(both, bb);
    composeGuarded(onlyElapsed, be);

    // The 0:00 case fits, so it DOES draw a remaining field; the 29:00 case must
    // not. Comparing them proves the long one was dropped rather than squeezed.
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, cmpRows(bb, be, kTimesY0, kTimesY1),
                                  "the two cases drew the same times band");
    // And what remains must be exactly the elapsed field, nothing further right
    // than it: rebuild it alone and require an exact match.
    uint8_t want[MediaPane::BYTES];
    memset(want, 0, sizeof(want));
    MediaPane::timeAt(want, 0, 0, 11 * 60);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmpRows(bb, want, kTimesY0, kTimesY1),
                                  "a remaining time was drawn that could not fit");
}

static void test_an_absurd_progress_ratio_does_not_iterate_millions_of_pixels()
{
    // The clamp on the fill is invisible in the PIXELS — px() clips, so an
    // over-long bar lights the same columns as a full one — which is what made
    // an earlier version of the overrun test pass with the clamp deleted. What
    // an unclamped fill actually costs is work: 46 * 65535 / 1 is three million
    // hline iterations inside a compose() that runs on the task owning the
    // panel's deadlines. hline/vline/rect now clip their own spans, so the loop
    // is bounded by the pane whatever it is handed. This asserts the result is
    // still correct at the extreme; the bound itself is structural.
    MediaPane p, full;
    for (MediaPane* q : {&p, &full}) {
        q->setScene(MediaPane::Scene::Media);
        q->setPlaying(true);
    }
    p.setProgress(65535, 1);
    full.setProgress(1, 1);

    uint8_t bp[MediaPane::BYTES], bf[MediaPane::BYTES];
    composeGuarded(p, bp);
    composeGuarded(full, bf);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cmpRows(bp, bf, kBarY0, kBarY1),
                                  "an absurd ratio drew something other than a full bar");
}

static void test_the_clock_ring_is_closed_rather_than_dashed()
{
    // circle() stepped a fixed 4 degrees, whose chord at the clock's radius is
    // 1.54 px — so the ring skipped pixels where the curve is steepest and drew
    // as a dashed circle broken at four points, while its comment claimed the
    // opposite. The step follows the radius now. Every row the ring passes
    // through must contain at least one lit pixel.
    MediaPane p;
    p.setScene(MediaPane::Scene::Clock);
    p.setClockSecs(0);
    uint8_t b[MediaPane::BYTES];
    composeGuarded(p, b);

    // The face is centred at (23,23) with r=22, so rows 1..45 all cross it.
    for (int y = 2; y <= 44; ++y) {
        bool any = false;
        for (int i = 0; i < MediaPane::STRIDE && !any; ++i)
            any = b[y * MediaPane::STRIDE + i] != 0;
        TEST_ASSERT_TRUE_MESSAGE(any, "the clock ring has a gap in a row");
    }
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_two_digit_remaining_time_is_not_truncated);
    RUN_TEST(test_two_long_times_drop_the_second_rather_than_overlapping);
    RUN_TEST(test_an_absurd_progress_ratio_does_not_iterate_millions_of_pixels);
    RUN_TEST(test_the_clock_ring_is_closed_rather_than_dashed);
    RUN_TEST(test_the_buffer_is_the_size_the_library_declares);
    RUN_TEST(test_the_top_left_pixel_is_bit_seven_of_byte_zero);
    RUN_TEST(test_pixels_outside_the_pane_are_dropped_not_wrapped);
    RUN_TEST(test_every_scene_round_trips_through_its_name);
    RUN_TEST(test_an_unknown_or_null_scene_name_falls_back_to_off);
    RUN_TEST(test_setting_a_scene_out_of_range_falls_back_to_off);
    RUN_TEST(test_only_media_and_tape_claim_to_use_media);
    RUN_TEST(test_no_scene_writes_outside_the_pane);
    RUN_TEST(test_a_pane_composed_before_it_was_ever_stepped_is_safe);
    RUN_TEST(test_off_composes_an_empty_pane);
    RUN_TEST(test_a_paused_track_settles_and_then_stops_changing);
    RUN_TEST(test_a_playing_track_keeps_changing);
    RUN_TEST(test_a_source_with_no_transport_drops_the_bands_it_cannot_fill);
    RUN_TEST(test_a_position_past_the_end_of_the_track_cannot_overrun_the_bar);
    RUN_TEST(test_a_zero_duration_draws_an_empty_bar_rather_than_dividing_by_it);
    RUN_TEST(test_a_track_longer_than_the_field_saturates_rather_than_wrapping);
    RUN_TEST(test_two_panes_stepped_alike_draw_alike);
    RUN_TEST(test_the_carousel_visits_every_scene_but_off_and_custom_and_wraps);
    RUN_TEST(test_the_custom_scene_draws_the_stored_picture_and_holds_still);
    RUN_TEST(test_the_picture_survives_a_round_trip_through_hex);
    RUN_TEST(test_a_bad_hex_picture_is_refused_whole);
    RUN_TEST(test_every_decorative_scene_moves_when_stepped);
    RUN_TEST(test_pictures_draw_and_the_tryzub_clock_tells_the_time);
    RUN_TEST(test_life_reseeds_rather_than_freezing);
    RUN_TEST(test_compose_does_not_advance_the_animation);
    RUN_TEST(test_the_clock_face_reads_the_time_it_was_given);
    RUN_TEST(test_the_clock_wraps_a_day_rather_than_running_off_the_face);
    RUN_TEST(test_the_tape_turns_only_while_playing);
    RUN_TEST(test_the_tape_winds_with_the_position);
    RUN_TEST(test_a_stale_gmeter_is_a_still_dotted_ring);
    RUN_TEST(test_the_gmeter_draws_the_dot_where_the_pull_is);
    RUN_TEST(test_the_gmeter_holds_the_peak_for_three_seconds);
    RUN_TEST(test_the_gmeter_drops_its_trail_when_the_data_goes);
    RUN_TEST(test_an_unverified_gmeter_moves_with_a_dotted_ring);
    RUN_TEST(test_accel_long_converts_to_milli_g);
    return UNITY_END();
}
