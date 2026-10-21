#include <unity.h>
#include <string.h>
#include "display/MediaScreen.h"
#include "display/TextFold.h"
#include "media/MediaInfo.h"

using MegaOpen::MediaScreen;

void setUp() {}
void tearDown() {}

struct Rows {
    char r1[MediaScreen::ROW_MAX];
    char r2[MediaScreen::ROW_MAX];
    char r3[MediaScreen::ROW_MAX];
};

static Rows draw(const MediaScreen& s, uint32_t nowMs)
{
    Rows r;
    s.compose(nowMs, r.r1, r.r2, r.r3);
    return r;
}

// Nothing may exceed the row width, ever. The panel truncates silently, so an
// over-long row does not fail — it just quietly loses its right-hand end, which
// is where the progress bar lives.
static void assertFits(const Rows& r, uint8_t cols)
{
    TEST_ASSERT_TRUE(strlen(r.r1) <= cols);
    TEST_ASSERT_TRUE(strlen(r.r2) <= cols);
    TEST_ASSERT_TRUE(strlen(r.r3) <= cols);
}

void test_short_strings_are_shown_whole_and_do_not_move()
{
    MediaScreen s;
    s.setTitle("Kashmir");
    s.setArtist("Led Zeppelin");
    s.setState(MediaScreen::State::Playing);
    s.setProgress(83, 225);

    const Rows a = draw(s, 0);
    TEST_ASSERT_EQUAL_STRING("Kashmir", a.r2);
    TEST_ASSERT_EQUAL_STRING("Led Zeppelin", a.r3);

    // Ten seconds later, with the scroll running, a string that fits must be
    // byte-identical. A short title that jitters is the classic marquee bug.
    const Rows b = draw(s, 10000);
    TEST_ASSERT_EQUAL_STRING(a.r2, b.r2);
    TEST_ASSERT_EQUAL_STRING(a.r3, b.r3);
}

void test_the_header_carries_state_time_and_a_bar()
{
    MediaScreen s;
    s.setState(MediaScreen::State::Playing);
    s.setProgress(83, 225);          // 1:23 of 3:45
    s.setBarWidth(10);

    const Rows r = draw(s, 0);
    TEST_ASSERT_EQUAL_STRING("> 1:23/3:45 [###-------]", r.r1);

    s.setState(MediaScreen::State::Paused);
    TEST_ASSERT_EQUAL_STRING("|| 1:23/3:45 [###-------]", draw(s, 0).r1);
    s.setState(MediaScreen::State::Rewinding);
    TEST_ASSERT_EQUAL_STRING("<< 1:23/3:45 [###-------]", draw(s, 0).r1);
    s.setState(MediaScreen::State::Forwarding);
    TEST_ASSERT_EQUAL_STRING(">> 1:23/3:45 [###-------]", draw(s, 0).r1);
}

// An unknown duration is a live radio stream or a source that does not report
// one. Showing "1:23/0:00" would be a lie and a full bar would be worse.
void test_unknown_duration_drops_the_total_and_leaves_the_bar_empty()
{
    MediaScreen s;
    s.setState(MediaScreen::State::Playing);
    s.setProgress(83, 0);
    const Rows r = draw(s, 0);
    TEST_ASSERT_EQUAL_STRING("> 1:23 [----------]", r.r1);
}

void test_the_bar_tracks_progress_and_never_overruns()
{
    MediaScreen s;
    s.setBarWidth(10);
    s.setState(MediaScreen::State::Playing);

    s.setProgress(0, 100);
    TEST_ASSERT_NOT_NULL(strstr(draw(s, 0).r1, "[----------]"));
    s.setProgress(50, 100);
    TEST_ASSERT_NOT_NULL(strstr(draw(s, 0).r1, "[#####-----]"));
    s.setProgress(100, 100);
    TEST_ASSERT_NOT_NULL(strstr(draw(s, 0).r1, "[##########]"));
    // Past the end — a source that over-reports must not run the bar off the row.
    s.setProgress(500, 100);
    TEST_ASSERT_NOT_NULL(strstr(draw(s, 0).r1, "[##########]"));
    assertFits(draw(s, 0), s.cols());
}

// The bar is dropped rather than squeezed when the row is narrow. A two-cell
// progress bar is not information, and stealing the time to fit it is worse.
void test_a_narrow_row_drops_the_bar_rather_than_truncating_the_time()
{
    MediaScreen s;
    s.setCols(12);
    s.setBarWidth(10);
    s.setState(MediaScreen::State::Playing);
    s.setProgress(83, 225);

    const Rows r = draw(s, 0);
    TEST_ASSERT_EQUAL_STRING("> 1:23/3:45", r.r1);
    assertFits(r, 12);
}

void test_minutes_are_not_clamped_to_two_digits()
{
    MediaScreen s;
    s.setState(MediaScreen::State::Playing);
    s.setBarWidth(0);
    s.setProgress(6012, 7200);      // 100:12 of 120:00 — a podcast
    TEST_ASSERT_EQUAL_STRING("> 100:12/120:00", draw(s, 0).r1);
}

void test_a_long_title_scrolls_and_wraps_through_a_visible_gap()
{
    MediaScreen s;
    s.setCols(10);
    s.setScrollMs(100);
    s.setTitle("ABCDEFGHIJKLMNOP");     // 16 > 10

    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", draw(s, 0).r2);
    TEST_ASSERT_EQUAL_STRING("BCDEFGHIJK", draw(s, 100).r2);
    TEST_ASSERT_EQUAL_STRING("CDEFGHIJKL", draw(s, 200).r2);

    // Every window is exactly `cols` wide, all the way round the wrap. The wrap
    // is where an off-by-one shows up, and it shows up as a short row rather
    // than as anything obviously wrong.
    for (uint32_t t = 0; t < 100 * 40; t += 100) {
        const Rows r = draw(s, t);
        TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)strlen(r.r2));
    }

    // And it comes back to the start: 16 chars + a 7-char gap = a period of 23.
    TEST_ASSERT_EQUAL_STRING(draw(s, 0).r2, draw(s, 100 * 23).r2);

    // The gap is visible, not an invisible seam — "...OPABC" would read as
    // corruption rather than as a wrap.
    bool sawSeparator = false;
    for (uint32_t t = 0; t < 100 * 23; t += 100)
        if (strchr(draw(s, t).r2, '-')) sawSeparator = true;
    TEST_ASSERT_TRUE(sawSeparator);
}

void test_scroll_zero_freezes_the_marquee()
{
    MediaScreen s;
    s.setCols(10);
    s.setScrollMs(0);
    s.setTitle("ABCDEFGHIJKLMNOP");

    // Frozen means frozen — this is the mode for "does the panel render at all",
    // where a moving row is one more thing that could be blamed.
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", draw(s, 0).r2);
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", draw(s, 999999).r2);
}

// Two rows of different lengths must not share one offset, or they drift in and
// out of phase and the screen reads as a rendering fault.
void test_title_and_artist_scroll_independently()
{
    MediaScreen s;
    s.setCols(6);
    s.setScrollMs(100);
    s.setTitle("ABCDEFGHIJ");     // 10
    s.setArtist("12345678901234");// 14

    // At a time that is a whole number of periods for the title but not for the
    // artist, the title is back at its start and the artist is not.
    const uint32_t titlePeriod = (10 + 7) * 100;
    TEST_ASSERT_EQUAL_STRING(draw(s, 0).r2, draw(s, titlePeriod).r2);
    TEST_ASSERT_TRUE(strcmp(draw(s, 0).r3, draw(s, titlePeriod).r3) != 0);
}

// The case the shared rate could not express, and the reason for the change.
// Two rows of the SAME length used to step in perfect lockstep — same offset,
// same instant, forever — which the eye reads as one wide row rather than two,
// and you lose which text is the title. Independent rates separate them whatever
// the strings are.
void test_rows_of_equal_length_still_drift_apart()
{
    MediaScreen s;
    s.setCols(6);
    s.setScrollMsTitle(700);
    s.setScrollMsArtist(1400);      // the defaults, stated so the test says what it means
    s.setTitle ("ABCDEFGHIJ");      // 10
    s.setArtist("1234567890");      // 10 — identical length, the hard case

    const Rows t0 = draw(s, 0);
    const Rows t1 = draw(s, 700);   // one title step, half an artist step

    TEST_ASSERT_TRUE(strcmp(t0.r2, t1.r2) != 0);   // the title has moved
    TEST_ASSERT_EQUAL_STRING(t0.r3, t1.r3);        // the artist has not
}

// Freezing one row must not freeze the other: "is the panel rendering at all"
// is easier to answer against one still row and one moving one than against two
// of either.
void test_one_row_can_be_frozen_while_the_other_scrolls()
{
    MediaScreen s;
    s.setCols(6);
    s.setScrollMsTitle(100);
    s.setScrollMsArtist(0);
    s.setTitle ("ABCDEFGHIJ");
    s.setArtist("1234567890");

    TEST_ASSERT_TRUE(strcmp(draw(s, 0).r2, draw(s, 5000).r2) != 0);
    TEST_ASSERT_EQUAL_STRING(draw(s, 0).r3, draw(s, 5000).r3);
}

// The broad setter is what the stored setting and the older API field mean, and
// both rows have to follow it or "freeze the scroll" only half works.
void test_the_broad_setter_moves_both_rows()
{
    MediaScreen s;
    s.setScrollMs(250);
    TEST_ASSERT_EQUAL_UINT16(250, s.scrollMsTitle());
    TEST_ASSERT_EQUAL_UINT16(250, s.scrollMsArtist());
    TEST_ASSERT_EQUAL_UINT16(250, s.scrollMs());    // reports the title row
}

void test_over_long_input_is_bounded_not_overflowed()
{
    MediaScreen s;
    char huge[512];
    memset(huge, 'X', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';

    s.setTitle(huge);
    s.setArtist(huge);
    s.setCols(31);
    const Rows r = draw(s, 12345);
    assertFits(r, 31);
    TEST_ASSERT_TRUE(strlen(s.title()) < MediaScreen::ROW_MAX * 2);
}

void test_null_strings_are_empty_rows_not_a_crash()
{
    MediaScreen s;
    s.setTitle(nullptr);
    s.setArtist(nullptr);
    const Rows r = draw(s, 0);
    TEST_ASSERT_EQUAL_STRING("", r.r2);
    TEST_ASSERT_EQUAL_STRING("", r.r3);
}

// dirty() is what stops a 13-frame repaint that would change nothing. Getting it
// backwards is invisible on a bench and floods the panel in a car.
void test_dirty_is_false_until_something_actually_changes()
{
    MediaScreen s;
    s.setTitle("Kashmir");
    s.setArtist("Led Zeppelin");
    s.setScrollMs(0);

    TEST_ASSERT_TRUE(s.dirty(0));        // never rendered yet
    s.markRendered(0);
    TEST_ASSERT_FALSE(s.dirty(0));
    TEST_ASSERT_FALSE(s.dirty(50000));   // frozen marquee: time alone changes nothing

    s.setTitle("Rain Song");
    TEST_ASSERT_TRUE(s.dirty(0));
    s.markRendered(0);
    TEST_ASSERT_FALSE(s.dirty(0));

    // A moving marquee IS a change, and has to be repainted.
    s.setCols(6);
    s.setScrollMs(100);
    s.markRendered(0);
    TEST_ASSERT_TRUE(s.dirty(100));
}

void test_progress_alone_makes_it_dirty()
{
    MediaScreen s;
    s.setScrollMs(0);
    s.setState(MediaScreen::State::Playing);
    s.setProgress(10, 200);
    s.markRendered(0);
    TEST_ASSERT_FALSE(s.dirty(0));
    s.setProgress(11, 200);
    TEST_ASSERT_TRUE(s.dirty(0));
}

// The buffer's last byte is never written, so it keeps its initial NUL forever.
// That is the bound a reader on another task relies on: the web preview composes
// on the HTTP task while the loop task is replacing a title, and copyBounded
// writes the terminator LAST — so mid-copy there is no terminator in the middle
// of the buffer at all. Only the reserved final byte stops strlen() there.
void test_the_last_byte_of_the_buffer_stays_a_terminator()
{
    MediaScreen s;
    char huge[512];
    memset(huge, 'X', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    s.setTitle(huge);
    s.setArtist(huge);

    // cap - 2, not cap - 1: the final byte is reserved.
    TEST_ASSERT_EQUAL_size_t(MediaScreen::ROW_MAX * 2 - 2, strlen(s.title()));
    TEST_ASSERT_EQUAL_size_t(MediaScreen::ROW_MAX * 2 - 2, strlen(s.artist()));
}

// ---------------------------------------------------------------------------
// The clock in the header
// ---------------------------------------------------------------------------

void test_the_clock_leads_the_header_when_it_is_set()
{
    MediaScreen s;
    s.setState(MediaScreen::State::Playing);
    s.setProgress(83, 225);
    s.setBarWidth(10);
    s.setClock("1234");

    // 26 columns: "[12:34] > 1:23/3:45" is 19, leaving room for a four-cell bar.
    const Rows r = draw(s, 0);
    TEST_ASSERT_EQUAL_STRING("[12:34] > 1:23/3:45 [#---]", r.r1);
    assertFits(r, 26);
}

// An unset clock leaves the header exactly as it was — no placeholder, no gap.
// A panel that has never heard from the phone should show no clock, not a
// convincing `[--:--]`.
void test_no_clock_leaves_the_header_untouched()
{
    MediaScreen s;
    s.setState(MediaScreen::State::Playing);
    s.setProgress(83, 225);
    s.setBarWidth(10);
    TEST_ASSERT_EQUAL_STRING("> 1:23/3:45 [###-------]", draw(s, 0).r1);
}

// ---- a source with no transport ---------------------------------------------
// The head unit reports no play state and no position anywhere in the Raise
// protocol. Left claiming one, row 1 reads "[12:34] || 0:00" — a pause glyph
// over a track that is playing and a position frozen at zero. That is not
// missing information, it is WRONG information, and on a dashboard the
// difference matters.

void test_without_transport_the_header_is_the_clock_alone()
{
    MediaScreen s;
    s.setHasTransport(false);
    s.setClock("1234");
    s.setState(MediaScreen::State::Playing);
    s.setProgress(83, 225);
    TEST_ASSERT_EQUAL_STRING("[12:34]", draw(s, 0).r1);
}

void test_without_transport_and_without_a_clock_row_one_is_empty()
{
    MediaScreen s;
    s.setHasTransport(false);
    s.setProgress(83, 225);
    TEST_ASSERT_EQUAL_STRING("", draw(s, 0).r1);
}

// The two text rows are the whole screen then, and they must still scroll.
void test_without_transport_the_text_rows_still_marquee()
{
    MediaScreen s;
    s.setHasTransport(false);
    s.setCols(10);
    s.setScrollMsTitle(100);
    s.setTitle("A Very Long Song Title Indeed");
    const Rows a = draw(s, 0);
    const Rows b = draw(s, 500);
    TEST_ASSERT_TRUE(strcmp(a.r2, b.r2) != 0);
}

void test_turning_transport_off_makes_the_screen_dirty()
{
    MediaScreen s;
    s.setClock("1234");
    s.setState(MediaScreen::State::Playing);
    s.setProgress(83, 225);
    s.markRendered(0);
    TEST_ASSERT_FALSE(s.dirty(0));
    s.setHasTransport(false);
    TEST_ASSERT_TRUE(s.dirty(0));
}

// ---- the ten styles ---------------------------------------------------------
// SWEPT, NOT ENUMERATED ONE BY ONE. What matters about a set of alternative
// screens is not what each one looks like — that is the point of being able to
// switch them — it is that NONE of them can break the panel. A row longer than
// the window is truncated silently by the glass, a missing terminator is a read
// off the end of the buffer, and a style that is not a pure function of
// (content, nowMs) makes dirty() true for ever and repaints at loop rate until
// the bus goes off. Those three are checked for all ten, on content chosen to be
// awkward.

static void fillTypical(MediaScreen& s)
{
    s.setTitle("Bohemian Rhapsody");
    s.setArtist("Queen");
    s.setClock("1234");
    s.setState(MediaScreen::State::Playing);
    s.setProgress(83, 225);
}

static void fillAwkward(MediaScreen& s)
{
    // Everything long, nothing empty, and a duration that makes the position
    // three digits wide.
    s.setTitle("A Ludicrously Long Song Title That Cannot Possibly Fit Anywhere");
    s.setArtist("An Equally Improbable Artist Name Of Great Length");
    s.setClock("2359");
    s.setState(MediaScreen::State::Playing);
    s.setProgress(6012, 7000);
}

void test_every_style_fits_the_row_and_terminates()
{
    for (uint8_t i = 0; i < MediaScreen::STYLE_COUNT; ++i) {
        for (uint8_t cols = 8; cols <= 26; cols += 6) {
            MediaScreen s;
            s.setStyle((MediaScreen::Style)i);
            s.setCols(cols);
            fillAwkward(s);
            for (uint32_t t = 0; t < 4000; t += 137) {
                const Rows r = draw(s, t);
                assertFits(r, cols);
            }
        }
    }
}

void test_every_style_survives_empty_content()
{
    // A freshly booted board with no phone: no title, no artist, no clock, no
    // duration. Three blank rows are acceptable; a crash or an overrun is not.
    for (uint8_t i = 0; i < MediaScreen::STYLE_COUNT; ++i) {
        MediaScreen s;
        s.setStyle((MediaScreen::Style)i);
        fillTypical(s);
        s.setTitle("");
        s.setArtist("");
        s.setClock("");
        s.setProgress(0, 0);
        for (uint32_t t = 0; t < 2000; t += 211) {
            const Rows r = draw(s, t);
            assertFits(r, s.cols());
        }
    }
}

void test_every_style_is_a_pure_function_of_time()
{
    // Called twice at the same instant, a style must produce the same bytes.
    // Anything else — a counter, a stored phase, a random — makes dirty() true
    // on every call, and the repaint loop then runs at whatever rate the wire
    // allows until something goes bus-off.
    for (uint8_t i = 0; i < MediaScreen::STYLE_COUNT; ++i) {
        MediaScreen s;
        s.setStyle((MediaScreen::Style)i);
        fillAwkward(s);
        for (uint32_t t = 0; t < 3000; t += 499) {
            const Rows a = draw(s, t);
            const Rows b = draw(s, t);
            TEST_ASSERT_EQUAL_STRING(a.r1, b.r1);
            TEST_ASSERT_EQUAL_STRING(a.r2, b.r2);
            TEST_ASSERT_EQUAL_STRING(a.r3, b.r3);
        }
    }
}

void test_a_still_screen_does_not_repaint_on_its_own()
{
    // The counterpart to styleAnimates(): a style that says it does not animate
    // must be clean for a full second on short content that has nothing to
    // scroll. Otherwise the cheapest screen on offer is not cheap.
    for (uint8_t i = 0; i < MediaScreen::STYLE_COUNT; ++i) {
        const MediaScreen::Style st = (MediaScreen::Style)i;
        if (MediaScreen::styleAnimates(st)) continue;
        MediaScreen s;
        s.setStyle(st);
        s.setTitle("Song");
        s.setArtist("Band");
        s.setClock("1234");
        s.setState(MediaScreen::State::Playing);
        s.setProgress(10, 200);
        s.markRendered(0);
        for (uint32_t t = 0; t <= 1000; t += 100)
            TEST_ASSERT_FALSE_MESSAGE(s.dirty(t), MediaScreen::styleName(st));
    }
}

void test_an_animated_style_actually_animates()
{
    for (uint8_t i = 0; i < MediaScreen::STYLE_COUNT; ++i) {
        const MediaScreen::Style st = (MediaScreen::Style)i;
        if (!MediaScreen::styleAnimates(st)) continue;
        MediaScreen s;
        s.setStyle(st);
        fillTypical(s);
        s.markRendered(0);
        bool moved = false;
        for (uint32_t t = 0; t <= 2000 && !moved; t += 50) moved = s.dirty(t);
        TEST_ASSERT_TRUE_MESSAGE(moved, MediaScreen::styleName(st));
    }
}

// A paused track must not animate: the whole reason Pulse exists is that its
// motion MEANS the transport is running.
void test_pulse_stands_still_when_paused()
{
    MediaScreen s;
    s.setStyle(MediaScreen::Style::Pulse);
    fillTypical(s);
    s.setState(MediaScreen::State::Paused);
    s.setTitle("Song");           // short, so nothing scrolls either
    s.setArtist("Band");
    s.markRendered(0);
    for (uint32_t t = 0; t <= 2000; t += 100) TEST_ASSERT_FALSE(s.dirty(t));
}

// Tape: clock, then two reels and the tape with the play head at the
// position; the spokes turn while playing and hold when paused.
void test_the_tape_reels_turn_only_while_playing()
{
    MediaScreen s;
    s.setStyle(MediaScreen::Style::Tape);
    s.setCols(22);
    fillTypical(s);                       // 83 s of 225, clock 12:34
    s.setProgress(0, 225);
    Rows r = draw(s, 0);
    // 22 - "12:34 " = 16: two 3-char reels and 10 of tape, head at its start.
    TEST_ASSERT_EQUAL_STRING("12:34 (+)o=========(+)", r.r1);
    TEST_ASSERT_EQUAL_STRING("Bohemian Rhapsody", r.r2);
    TEST_ASSERT_EQUAL_STRING("Queen", r.r3);
    TEST_ASSERT_EQUAL_STRING("12:34 (x)o=========(x)", draw(s, 250).r1);
    s.setProgress(225, 225);              // the end: the head on the last cell
    TEST_ASSERT_EQUAL_STRING("12:34 (+)=========o(+)", draw(s, 0).r1);
    s.setState(MediaScreen::State::Paused);
    TEST_ASSERT_EQUAL_STRING(draw(s, 0).r1, draw(s, 250).r1);
}

// At the info layout's 8 columns there is no room for the clock: the
// cassette alone. With no transport (the head unit) there is no head.
void test_the_tape_fits_eight_columns_and_needs_no_position()
{
    MediaScreen s;
    s.setStyle(MediaScreen::Style::Tape);
    s.setCols(8);
    fillTypical(s);
    s.setHasTransport(false);
    TEST_ASSERT_EQUAL_STRING("(+)==(+)", draw(s, 0).r1);
    // No state from the head unit: a title is enough to turn the reels.
    TEST_ASSERT_EQUAL_STRING("(x)==(x)", draw(s, 250).r1);
}

// Typewriter: a new title types in a letter per TYPE_MS behind a caret, then
// the marquee starts from its first character, not mid-string.
void test_the_typewriter_types_a_new_title_then_scrolls_from_the_start()
{
    MediaScreen s;
    s.setStyle(MediaScreen::Style::Typewriter);
    s.setCols(10);
    fillTypical(s);
    s.setTitle("ABCDEFGHIJKLMNOP");       // longer than a row
    s.setScrollMsTitle(100);
    const uint32_t t0 = 5000;
    s.restartMarquee(t0);
    TEST_ASSERT_EQUAL_STRING("_", draw(s, t0).r2);
    TEST_ASSERT_EQUAL_STRING("ABC_", draw(s, t0 + 3 * MediaScreen::TYPE_MS).r2);
    // Nine letters (a row less the caret), then the marquee, held at the start
    // for its lead.
    const uint32_t typed = t0 + 9 * MediaScreen::TYPE_MS;
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", draw(s, typed).r2);
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", draw(s, typed + MediaScreen::MARQUEE_LEAD_MS - 1).r2);
    TEST_ASSERT_EQUAL_STRING("BCDEFGHIJK", draw(s, typed + MediaScreen::MARQUEE_LEAD_MS + 100).r2);
    // Artist on top, the transport below.
    const Rows r = draw(s, typed);
    TEST_ASSERT_EQUAL_STRING("Queen", r.r1);
    TEST_ASSERT_EQUAL_INT('[', r.r3[0]);
}

// No track change seen (boot, a style switched mid-song): nothing to type.
void test_the_typewriter_without_a_track_change_shows_the_title()
{
    MediaScreen s;
    s.setStyle(MediaScreen::Style::Typewriter);
    fillTypical(s);
    TEST_ASSERT_EQUAL_STRING("Bohemian Rhapsody", draw(s, 0).r2);
    TEST_ASSERT_FALSE(MediaScreen::styleAnimates(MediaScreen::Style::Typewriter));
    TEST_ASSERT_TRUE(MediaScreen::styleAnimates(MediaScreen::Style::Tape));
}

void test_style_names_round_trip_and_reject_junk()
{
    for (uint8_t i = 0; i < MediaScreen::STYLE_COUNT; ++i) {
        const MediaScreen::Style st = (MediaScreen::Style)i;
        TEST_ASSERT_EQUAL_UINT8(i,
            (uint8_t)MediaScreen::styleFromName(MediaScreen::styleName(st)));
    }
    // Unknown falls back to the default rather than to whatever index 0 of a
    // half-parsed string happened to be.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MediaScreen::Style::Classic,
                            (uint8_t)MediaScreen::styleFromName("nonsense"));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MediaScreen::Style::Classic,
                            (uint8_t)MediaScreen::styleFromName(nullptr));
}

// The ticker is one band through three rows, not three independent lines: what
// leaves the right of row 1 arrives at the left of row 2.
void test_the_ticker_reads_as_one_continuous_band()
{
    MediaScreen s;
    s.setStyle(MediaScreen::Style::Ticker);
    s.setCols(10);
    s.setTitle("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    s.setArtist("0123456789");
    s.setScrollMsTitle(100);
    const Rows r = draw(s, 0);
    // Row 2 begins where row 1 ended.
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", r.r1);
    TEST_ASSERT_EQUAL_STRING("KLMNOPQRST", r.r2);
}

void test_bigtitle_wraps_on_a_space_rather_than_mid_word()
{
    MediaScreen s;
    s.setStyle(MediaScreen::Style::BigTitle);
    s.setCols(12);
    s.setTitle("Bohemian Rhapsody");
    s.setArtist("Queen");
    const Rows r = draw(s, 0);
    TEST_ASSERT_EQUAL_STRING("Bohemian", r.r1);
    TEST_ASSERT_EQUAL_STRING("Rhapsody", r.r2);
    TEST_ASSERT_EQUAL_STRING("Queen", r.r3);
}

void test_the_clock_style_letter_spaces_the_time()
{
    MediaScreen s;
    s.setStyle(MediaScreen::Style::Clock);
    s.setCols(20);
    s.setClock("1234");
    const Rows r = draw(s, 0);
    TEST_ASSERT_NOT_NULL(strstr(r.r1, "1 2 : 3 4"));
}

void test_the_clock_style_falls_back_to_the_position()
{
    MediaScreen s;
    s.setStyle(MediaScreen::Style::Clock);
    s.setCols(20);
    s.setProgress(83, 225);
    const Rows r = draw(s, 0);
    TEST_ASSERT_NOT_NULL(strstr(r.r1, "1:23/3:45"));
}

// Switching style must repaint even though nothing about the content changed.
void test_changing_style_makes_the_screen_dirty()
{
    MediaScreen s;
    fillTypical(s);
    s.markRendered(0);
    TEST_ASSERT_FALSE(s.dirty(0));
    s.setStyle(MediaScreen::Style::Minimal);
    TEST_ASSERT_TRUE(s.dirty(0));
}

void test_an_out_of_range_style_falls_back_rather_than_indexing_past_the_table()
{
    MediaScreen s;
    s.setStyle((MediaScreen::Style)200);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MediaScreen::Style::Classic, (uint8_t)s.style());
}

// Every style must still respect a source with no transport — none of them may
// invent a play state or a position the head unit never sent.
void test_no_style_invents_a_position_without_transport()
{
    for (uint8_t i = 0; i < MediaScreen::STYLE_COUNT; ++i) {
        MediaScreen s;
        s.setStyle((MediaScreen::Style)i);
        s.setHasTransport(false);
        s.setTitle("Song");
        s.setArtist("Band");
        s.setProgress(83, 225);      // set, and must not be drawn
        for (uint32_t t = 0; t < 1500; t += 250) {
            const Rows r = draw(s, t);
            TEST_ASSERT_NULL(strstr(r.r1, "1:23"));
            TEST_ASSERT_NULL(strstr(r.r2, "1:23"));
            TEST_ASSERT_NULL(strstr(r.r3, "1:23"));
        }
    }
}

void test_the_clock_accepts_both_spellings_and_rejects_junk()
{
    MediaScreen s;
    s.setClock("1234");
    TEST_ASSERT_EQUAL_STRING("12:34", s.clock());
    s.setClock("07:05");
    TEST_ASSERT_EQUAL_STRING("07:05", s.clock());

    // Anything that is not a real time clears it, so the header shows nothing
    // rather than something wrong.
    s.setClock("2534");  TEST_ASSERT_EQUAL_STRING("", s.clock());   // hour 25
    s.setClock("1275");  TEST_ASSERT_EQUAL_STRING("", s.clock());   // minute 75
    s.setClock("12");    TEST_ASSERT_EQUAL_STRING("", s.clock());   // too short
    s.setClock("ab:cd"); TEST_ASSERT_EQUAL_STRING("", s.clock());
    s.setClock("");      TEST_ASSERT_EQUAL_STRING("", s.clock());
    s.setClock(nullptr); TEST_ASSERT_EQUAL_STRING("", s.clock());
}

// The clock costs eight columns off the front of the row. At the default bar
// width of 10 there is no longer room for it, and the bar used to be all-or-
// nothing — so it silently disappeared the moment the phone first reported the
// time. It shrinks now.
void test_the_bar_shrinks_to_fit_rather_than_disappearing()
{
    MediaScreen s;
    s.setState(MediaScreen::State::Playing);
    s.setProgress(0, 225);
    s.setBarWidth(10);
    s.setClock("1234");
    TEST_ASSERT_NOT_NULL(strchr(draw(s, 0).r1, '['));
    TEST_ASSERT_EQUAL_STRING("[12:34] > 0:00/3:45 [----]", draw(s, 0).r1);
}

// Below three cells it is still dropped: a two-cell bar can only read empty or
// full, which is a decoration rather than information.
void test_a_bar_with_no_room_is_still_dropped_entirely()
{
    MediaScreen s;
    s.setCols(20);
    s.setBarWidth(10);
    s.setState(MediaScreen::State::Playing);
    s.setProgress(83, 225);
    s.setClock("1234");
    const Rows r = draw(s, 0);
    TEST_ASSERT_EQUAL_STRING("[12:34] > 1:23/3:45", r.r1);
    assertFits(r, 20);
}

void test_the_clock_alone_makes_it_dirty()
{
    MediaScreen s;
    s.setScrollMs(0);
    s.markRendered(0);
    TEST_ASSERT_FALSE(s.dirty(0));
    s.setClock("1234");
    TEST_ASSERT_TRUE(s.dirty(0));
}

// ---------------------------------------------------------------------------
// MediaInfo::elapsedAt — the extrapolation, and its three bounds
// ---------------------------------------------------------------------------
// AMS reports the position when it CHANGES, which during steady playback is
// never. Everything the progress bar shows between updates is this function's
// guess, and nothing ever comes back to correct it.

void test_elapsed_advances_at_the_reported_rate_while_playing()
{
    MediaInfo m;
    m.playbackState = MediaInfo::PlaybackState::Playing;
    m.playbackRate = 1.0f;
    m.elapsedTime = 10.0f;
    m.duration = 200.0f;
    m.lastUpdateMs = 5000;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, m.elapsedAt(35000));
}

void test_a_paused_track_does_not_advance()
{
    MediaInfo m;
    m.playbackState = MediaInfo::PlaybackState::Paused;
    m.playbackRate = 1.0f;
    m.elapsedTime = 10.0f;
    m.lastUpdateMs = 5000;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, m.elapsedAt(600000));
}

// The phone goes away mid-song: nothing tells us playback stopped, so the guess
// runs forever. Parked at the end of the track is the only honest answer — a bar
// reading 9:14/3:45 is worse than one stuck at 3:45, because only the second is
// obviously the truth.
void test_the_position_never_runs_past_the_end_of_the_track()
{
    MediaInfo m;
    m.playbackState = MediaInfo::PlaybackState::Playing;
    m.playbackRate = 1.0f;
    m.elapsedTime = 10.0f;
    m.duration = 200.0f;
    m.lastUpdateMs = 0 + 1;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 200.0f, m.elapsedAt(1 + 3600000));
}

// An unknown duration cannot be clamped against, so it is left alone rather than
// clamped to zero — a podcast with no reported length still counts up.
void test_an_unknown_duration_is_not_clamped_to_zero()
{
    MediaInfo m;
    m.playbackState = MediaInfo::PlaybackState::Playing;
    m.playbackRate = 1.0f;
    m.elapsedTime = 10.0f;
    m.duration = 0.0f;
    m.lastUpdateMs = 1000;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 70.0f, m.elapsedAt(61000));
}

// lastUpdateMs sampled a moment AHEAD of nowMs — two callers reading millis()
// microseconds apart — makes the unsigned subtraction wrap to ~49 days instead
// of a small negative. At 1x that is a track position of four billion seconds.
void test_a_timestamp_from_the_future_is_not_extrapolated()
{
    MediaInfo m;
    m.playbackState = MediaInfo::PlaybackState::Playing;
    m.playbackRate = 1.0f;
    m.elapsedTime = 42.0f;
    m.duration = 0.0f;             // no clamp available to hide the damage
    m.lastUpdateMs = 10000;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.0f, m.elapsedAt(9990));
}

// A negative rate would otherwise produce a negative float, and the caller casts
// to uint16_t — which turns -1 into 65535 and puts 1092:15 on the glass.
void test_a_negative_rate_cannot_drive_the_position_below_zero()
{
    MediaInfo m;
    m.playbackState = MediaInfo::PlaybackState::Playing;
    m.playbackRate = -2.0f;
    m.elapsedTime = 5.0f;
    m.duration = 200.0f;
    m.lastUpdateMs = 1000;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, m.elapsedAt(61000));
}

// ---- flow: the two text rows as one block ----------------------------------------
static MediaScreen flowScreen()
{
    MediaScreen s;
    s.setStyle(MediaScreen::Style::Flow);
    s.setClock("1234");
    s.setState(MediaScreen::State::Playing);
    s.setProgress(83, 225);
    return s;
}

void test_flow_opens_with_exactly_classics_header()
{
    MediaScreen c; c.setStyle(MediaScreen::Style::Classic);
    c.setClock("1234"); c.setState(MediaScreen::State::Playing); c.setProgress(83, 225);
    c.setTitle("Kashmir"); c.setArtist("Led Zeppelin");
    MediaScreen f = flowScreen();
    f.setTitle("Kashmir"); f.setArtist("Led Zeppelin");
    // The clock is the point: whichever style is up, the time is in the same place.
    TEST_ASSERT_EQUAL_STRING(draw(c, 0).r1, draw(f, 0).r1);
    TEST_ASSERT_NOT_NULL(strstr(draw(f, 0).r1, "[12:34]"));
}

void test_flow_with_a_title_that_fits_is_still_and_shows_the_album_beside_the_artist()
{
    MediaScreen s = flowScreen();
    s.setTitle("Kashmir"); s.setArtist("Led Zeppelin"); s.setAlbum("Graffiti");
    const Rows a = draw(s, 0);
    TEST_ASSERT_EQUAL_STRING("Kashmir", a.r2);
    TEST_ASSERT_EQUAL_STRING("Led Zeppelin - Graffiti", a.r3);
    // Nothing moves and nothing pages: a minute later it is the same screen, so
    // it costs the bus nothing beyond the header's own seconds.
    const Rows b = draw(s, 60000);
    TEST_ASSERT_EQUAL_STRING(a.r2, b.r2);
    TEST_ASSERT_EQUAL_STRING(a.r3, b.r3);
}

void test_flow_drops_the_album_rather_than_scroll_when_the_pair_does_not_fit()
{
    MediaScreen s = flowScreen();
    s.setTitle("Kashmir"); s.setArtist("Led Zeppelin"); s.setAlbum("Physical Graffiti (Deluxe)");
    TEST_ASSERT_EQUAL_STRING("Led Zeppelin", draw(s, 0).r3);
}

void test_flow_wraps_a_long_title_onto_the_second_row_instead_of_scrolling()
{
    MediaScreen s = flowScreen();
    s.setTitle("The Rain Song (Remastered 2014 Version)");        // 39 > 26
    s.setArtist("Led Zeppelin");
    const Rows r = draw(s, 0);
    assertFits(r, 26);
    TEST_ASSERT_EQUAL_STRING("The Rain Song (Remastered", r.r2);  // broken at a space
    TEST_ASSERT_EQUAL_STRING("2014 Version)", r.r3);              // and ALL of it is there
    // Still, for the whole of the title page — this is the saving over a marquee.
    const Rows later = draw(s, MediaScreen::FLOW_TITLE_MS - 1);
    TEST_ASSERT_EQUAL_STRING(r.r2, later.r2);
    TEST_ASSERT_EQUAL_STRING(r.r3, later.r3);
}

void test_flow_gives_the_artist_and_album_their_own_page()
{
    MediaScreen s = flowScreen();
    s.setTitle("The Rain Song (Remastered 2014 Version)");
    s.setArtist("Led Zeppelin"); s.setAlbum("Houses of the Holy");
    const Rows info = draw(s, MediaScreen::FLOW_TITLE_MS);
    TEST_ASSERT_EQUAL_STRING("Led Zeppelin", info.r2);
    TEST_ASSERT_EQUAL_STRING("Houses of the Holy", info.r3);
    // ...and back to the title when the cycle comes round.
    const Rows again = draw(s, MediaScreen::FLOW_TITLE_MS + MediaScreen::FLOW_INFO_MS);
    TEST_ASSERT_EQUAL_STRING("The Rain Song (Remastered", again.r2);
}

void test_flow_does_not_page_to_a_blank_screen_when_there_is_no_artist()
{
    MediaScreen s = flowScreen();
    s.setTitle("The Rain Song (Remastered 2014 Version)");
    const Rows r = draw(s, MediaScreen::FLOW_TITLE_MS + 10);      // where the info page would be
    TEST_ASSERT_EQUAL_STRING("The Rain Song (Remastered", r.r2);
}

void test_flow_cuts_mid_word_only_when_no_space_leaves_a_row_that_fits()
{
    MediaScreen s = flowScreen();
    // The only space is at 3: breaking there leaves 40 characters for a 26-column
    // row. BigTitle would take it and lose the end; this must not.
    s.setTitle("An Extraordinarilylongunbrokenwordfollowsit");   // 43
    const Rows r = draw(s, 0);
    assertFits(r, 26);
    TEST_ASSERT_EQUAL_UINT(26, strlen(r.r2));
    TEST_ASSERT_EQUAL_STRING("followsit", r.r3 + strlen(r.r3) - 9);  // the END survived
}

void test_flow_holds_the_opening_and_scrolls_the_rest_past_two_rows()
{
    MediaScreen s = flowScreen();
    s.setTitle("Symphony No. 9 in D minor, Op. 125 Choral: IV. Presto - Allegro");
    const Rows a = draw(s, 0), b = draw(s, 2100);
    assertFits(a, 26); assertFits(b, 26);
    TEST_ASSERT_EQUAL_STRING("Symphony No. 9 in D minor,", a.r2);
    TEST_ASSERT_EQUAL_STRING(a.r2, b.r2);             // the part that names it stays put
    TEST_ASSERT_TRUE(strcmp(a.r3, b.r3) != 0);        // the remainder moves
}

void test_flow_does_not_print_an_album_that_is_just_the_title_again()
{
    // A single: the album IS the track. The first thing this style was run
    // against on real glass was `Karpati Lviv` from the album `Karpati Lviv`.
    MediaScreen s = flowScreen();
    s.setTitle("Karpati Lviv"); s.setArtist("Ulyana Del Rej"); s.setAlbum("KARPATI LVIV");
    TEST_ASSERT_EQUAL_STRING("Ulyana Del Rej", draw(s, 0).r3);
}

// ---- the track splash, and the width that makes it necessary -------------------
static const char* ltrim(const char* s) { while (*s == ' ') ++s; return s; }

void test_the_info_layout_is_eight_columns_and_the_others_twenty_six()
{
    // Measured on glass: `Potyag na Pivden` read `Potyag n` beside the pane.
    TEST_ASSERT_EQUAL_UINT8(8,  MediaScreen::colsFor(MediaScreen::Layout::Info));
    TEST_ASSERT_EQUAL_UINT8(8,  MediaScreen::colsFor(MediaScreen::Layout::Text));
    TEST_ASSERT_EQUAL_UINT8(26, MediaScreen::colsFor(MediaScreen::Layout::Fullscreen));
    TEST_ASSERT_EQUAL_UINT8(26, MediaScreen::colsFor(MediaScreen::Layout::Menu));
    // The two eight-column layouts are exactly the two that share the glass
    // with the pane, and the splash gate in main.cpp keys off this.
    TEST_ASSERT_TRUE(MediaScreen::sharesWithPane(MediaScreen::Layout::Info));
    TEST_ASSERT_TRUE(MediaScreen::sharesWithPane(MediaScreen::Layout::Text));
    TEST_ASSERT_FALSE(MediaScreen::sharesWithPane(MediaScreen::Layout::Menu));
    TEST_ASSERT_FALSE(MediaScreen::sharesWithPane(MediaScreen::Layout::Fullscreen));
    // Stored by name; a typo must not land on the layout that hides the pane
    // by way of the new entry either.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MediaScreen::Layout::Text,
                            (uint8_t)MediaScreen::layoutFromName("text"));
    TEST_ASSERT_EQUAL_STRING("text", MediaScreen::layoutName(MediaScreen::Layout::Text));
}

void test_a_new_track_starts_the_marquee_at_its_first_character()
{
    MediaScreen s;
    s.setLayout(MediaScreen::Layout::Text);
    s.setCols(MediaScreen::colsFor(MediaScreen::Layout::Text));
    s.setTitle("Potyag na Pivden"); s.setArtist("Zwyntar");

    // Mid-scroll on the wall clock, a new track arrives.
    const uint32_t t0 = (uint32_t)s.scrollMsTitle() * 13 + 123;
    TEST_ASSERT_TRUE(strcmp("Na rakhu", draw(s, t0).r1) != 0);
    s.setTitle("Na rakhunok tri"); s.setArtist("Tember Blanche");
    s.restartMarquee(t0);

    // Held at the first character for the lead, then it moves.
    TEST_ASSERT_EQUAL_STRING("Na rakhu", draw(s, t0).r1);
    TEST_ASSERT_EQUAL_STRING("Na rakhu", draw(s, t0 + MediaScreen::MARQUEE_LEAD_MS - 1).r1);
    TEST_ASSERT_EQUAL_STRING("a rakhun",
        draw(s, t0 + MediaScreen::MARQUEE_LEAD_MS + s.scrollMsTitle()).r1);
}

void test_prefold_maps_apostrophes_and_drops_emoji()
{
    char out[64];
    MegaOpen::preFold("p\xE2\x80\x99" "yatij kadr", out, sizeof(out));           // U+2019
    TEST_ASSERT_EQUAL_STRING("p'yatij kadr", out);
    MegaOpen::preFold("p\xCA\xBC" "yatij", out, sizeof(out));                    // U+02BC
    TEST_ASSERT_EQUAL_STRING("p'yatij", out);
    MegaOpen::preFold("\xF0\x9F\x98\x80 Vi: test \xF0\x9F\x91\x8D ok", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Vi: test ok", out);
    MegaOpen::preFold("\xE2\x9D\xA4\xEF\xB8\x8F love", out, sizeof(out));         // heart + VS16
    TEST_ASSERT_EQUAL_STRING("love", out);
    MegaOpen::preFold("a \xE2\x80\x94 b\xE2\x80\xA6", out, sizeof(out));         // em dash, ellipsis
    TEST_ASSERT_EQUAL_STRING("a - b...", out);
    MegaOpen::preFold("\xD0\x9F\xD1\x80\xD0\xB8", out, sizeof(out));              // Cyrillic untouched
    TEST_ASSERT_EQUAL_STRING("\xD0\x9F\xD1\x80\xD0\xB8", out);
    MegaOpen::preFold(nullptr, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
    MegaOpen::preFold("abcdefgh", out, 5);                                        // bounded
    TEST_ASSERT_EQUAL_STRING("abcd", out);
}

void test_the_text_layout_is_one_scrolling_row_whatever_the_style_says()
{
    // The panel's main line: eight characters, one message. The style cannot
    // apply — none of the ten fits in one row — so compose() overrides it, and
    // rows 2 and 3 come back empty rather than as the style's leftovers.
    MediaScreen s;
    s.setLayout(MediaScreen::Layout::Text);
    s.setCols(MediaScreen::colsFor(MediaScreen::Layout::Text));
    s.setTitle("Potyag na Pivden"); s.setArtist("Zwyntar");
    s.setStyle(MediaScreen::Style::Vu);           // an animated three-row style

    Rows a = draw(s, 0);
    TEST_ASSERT_EQUAL_STRING("Potyag n", a.r1);
    TEST_ASSERT_EQUAL_STRING("", a.r2);
    TEST_ASSERT_EQUAL_STRING("", a.r3);
    // It scrolls on the title clock, and the artist follows in the same band.
    Rows b = draw(s, (uint32_t)s.scrollMsTitle() * 20);
    TEST_ASSERT_EQUAL_UINT(8, strlen(b.r1));
    TEST_ASSERT_TRUE_MESSAGE(strcmp(a.r1, b.r1) != 0, "the text line did not scroll");
    bool sawArtist = false;
    for (uint32_t t = 0; t < (uint32_t)s.scrollMsTitle() * 40 && !sawArtist; t += s.scrollMsTitle())
        sawArtist = strstr(draw(s, t).r1, "Zwyn") != nullptr;
    TEST_ASSERT_TRUE_MESSAGE(sawArtist, "the artist never came round in the band");

    // A track that fits is STILL — the marquee is for what does not fit.
    s.setTitle("Radio"); s.setArtist("");
    TEST_ASSERT_EQUAL_STRING("Radio", draw(s, 0).r1);
    TEST_ASSERT_EQUAL_STRING("Radio", draw(s, 99999).r1);
    // Nothing yet is the same "---" the other layouts show, not an empty line.
    s.setTitle("");
    TEST_ASSERT_EQUAL_STRING("---", draw(s, 0).r1);
}

void test_the_splash_is_full_width_even_when_the_screen_is_eight_columns()
{
    MediaScreen s = flowScreen();
    s.setLayout(MediaScreen::Layout::Info);
    s.setCols(MediaScreen::colsFor(MediaScreen::Layout::Info));
    s.setTitle("Potyag na Pivden"); s.setArtist("Zwyntar"); s.setAlbum("Dykyi Skhid");

    // The regular screen cannot show it...
    TEST_ASSERT_TRUE(strlen(draw(s, 0).r2) <= 8);
    // ...and the splash shows all three, whole, centred in twenty-six.
    Rows r; s.composeSplash(r.r1, r.r2, r.r3);
    TEST_ASSERT_EQUAL_UINT(26, strlen(r.r1));
    TEST_ASSERT_EQUAL_STRING_LEN("Potyag na Pivden", ltrim(r.r1), 16);
    TEST_ASSERT_EQUAL_STRING_LEN("Zwyntar", ltrim(r.r2), 7);
    TEST_ASSERT_EQUAL_STRING_LEN("Dykyi Skhid", ltrim(r.r3), 11);
    TEST_ASSERT_EQUAL_CHAR(' ', r.r1[0]);             // centred, not left-aligned
}

void test_the_splash_gives_a_long_title_two_rows_and_moves_the_artist_down()
{
    MediaScreen s = flowScreen();
    s.setTitle("The Rain Song (Remastered 2014 Version)"); s.setArtist("Led Zeppelin");
    s.setAlbum("Houses of the Holy");
    Rows r; s.composeSplash(r.r1, r.r2, r.r3);
    assertFits(r, 26);
    TEST_ASSERT_EQUAL_STRING_LEN("The Rain Song (Remastered", ltrim(r.r1), 25);
    TEST_ASSERT_EQUAL_STRING_LEN("2014 Version)", ltrim(r.r2), 13);
    TEST_ASSERT_EQUAL_STRING_LEN("Led Zeppelin", ltrim(r.r3), 12);
}

void test_the_splash_does_not_repeat_a_title_as_its_album()
{
    MediaScreen s = flowScreen();
    s.setTitle("Karpati Lviv"); s.setArtist("Ulyana Del Rej"); s.setAlbum("Karpati Lviv");
    Rows r; s.composeSplash(r.r1, r.r2, r.r3);
    TEST_ASSERT_EQUAL_STRING("", ltrim(r.r3));
}

void test_invalidate_forces_a_repaint_of_rows_that_did_not_change()
{
    // What the splash needs on its way out: a fullscreen drew over the glass,
    // and the rows underneath are byte for byte what was last rendered.
    MediaScreen s = flowScreen();
    s.setTitle("Kashmir"); s.setArtist("Led Zeppelin");
    s.markRendered(1000);
    TEST_ASSERT_FALSE(s.dirty(1000));
    s.invalidate();
    TEST_ASSERT_TRUE(s.dirty(1000));
    s.markRendered(1000);
    TEST_ASSERT_FALSE(s.dirty(1000));
}

void test_flow_is_one_of_the_styles_by_name()
{
    TEST_ASSERT_TRUE(MediaScreen::styleFromName("flow") == MediaScreen::Style::Flow);
    TEST_ASSERT_EQUAL_STRING("flow", MediaScreen::styleName(MediaScreen::Style::Flow));
    TEST_ASSERT_FALSE(MediaScreen::styleAnimates(MediaScreen::Style::Flow));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_strings_are_shown_whole_and_do_not_move);
    RUN_TEST(test_the_header_carries_state_time_and_a_bar);
    RUN_TEST(test_unknown_duration_drops_the_total_and_leaves_the_bar_empty);
    RUN_TEST(test_the_bar_tracks_progress_and_never_overruns);
    RUN_TEST(test_a_narrow_row_drops_the_bar_rather_than_truncating_the_time);
    RUN_TEST(test_minutes_are_not_clamped_to_two_digits);
    RUN_TEST(test_a_long_title_scrolls_and_wraps_through_a_visible_gap);
    RUN_TEST(test_scroll_zero_freezes_the_marquee);
    RUN_TEST(test_title_and_artist_scroll_independently);
    RUN_TEST(test_rows_of_equal_length_still_drift_apart);
    RUN_TEST(test_one_row_can_be_frozen_while_the_other_scrolls);
    RUN_TEST(test_the_broad_setter_moves_both_rows);
    RUN_TEST(test_over_long_input_is_bounded_not_overflowed);
    RUN_TEST(test_null_strings_are_empty_rows_not_a_crash);
    RUN_TEST(test_dirty_is_false_until_something_actually_changes);
    RUN_TEST(test_progress_alone_makes_it_dirty);
    RUN_TEST(test_the_last_byte_of_the_buffer_stays_a_terminator);
    RUN_TEST(test_the_clock_leads_the_header_when_it_is_set);
    RUN_TEST(test_no_clock_leaves_the_header_untouched);
    RUN_TEST(test_every_style_fits_the_row_and_terminates);
    RUN_TEST(test_every_style_survives_empty_content);
    RUN_TEST(test_every_style_is_a_pure_function_of_time);
    RUN_TEST(test_a_still_screen_does_not_repaint_on_its_own);
    RUN_TEST(test_an_animated_style_actually_animates);
    RUN_TEST(test_pulse_stands_still_when_paused);
    RUN_TEST(test_style_names_round_trip_and_reject_junk);
    RUN_TEST(test_the_ticker_reads_as_one_continuous_band);
    RUN_TEST(test_bigtitle_wraps_on_a_space_rather_than_mid_word);
    RUN_TEST(test_the_clock_style_letter_spaces_the_time);
    RUN_TEST(test_the_clock_style_falls_back_to_the_position);
    RUN_TEST(test_changing_style_makes_the_screen_dirty);
    RUN_TEST(test_an_out_of_range_style_falls_back_rather_than_indexing_past_the_table);
    RUN_TEST(test_no_style_invents_a_position_without_transport);
    RUN_TEST(test_without_transport_the_header_is_the_clock_alone);
    RUN_TEST(test_without_transport_and_without_a_clock_row_one_is_empty);
    RUN_TEST(test_without_transport_the_text_rows_still_marquee);
    RUN_TEST(test_turning_transport_off_makes_the_screen_dirty);
    RUN_TEST(test_the_clock_accepts_both_spellings_and_rejects_junk);
    RUN_TEST(test_the_bar_shrinks_to_fit_rather_than_disappearing);
    RUN_TEST(test_a_bar_with_no_room_is_still_dropped_entirely);
    RUN_TEST(test_the_clock_alone_makes_it_dirty);
    RUN_TEST(test_elapsed_advances_at_the_reported_rate_while_playing);
    RUN_TEST(test_a_paused_track_does_not_advance);
    RUN_TEST(test_the_position_never_runs_past_the_end_of_the_track);
    RUN_TEST(test_an_unknown_duration_is_not_clamped_to_zero);
    RUN_TEST(test_a_timestamp_from_the_future_is_not_extrapolated);
    RUN_TEST(test_a_negative_rate_cannot_drive_the_position_below_zero);
    RUN_TEST(test_flow_opens_with_exactly_classics_header);
    RUN_TEST(test_flow_with_a_title_that_fits_is_still_and_shows_the_album_beside_the_artist);
    RUN_TEST(test_flow_drops_the_album_rather_than_scroll_when_the_pair_does_not_fit);
    RUN_TEST(test_flow_wraps_a_long_title_onto_the_second_row_instead_of_scrolling);
    RUN_TEST(test_flow_gives_the_artist_and_album_their_own_page);
    RUN_TEST(test_flow_does_not_page_to_a_blank_screen_when_there_is_no_artist);
    RUN_TEST(test_flow_cuts_mid_word_only_when_no_space_leaves_a_row_that_fits);
    RUN_TEST(test_flow_holds_the_opening_and_scrolls_the_rest_past_two_rows);
    RUN_TEST(test_flow_does_not_print_an_album_that_is_just_the_title_again);
    RUN_TEST(test_the_info_layout_is_eight_columns_and_the_others_twenty_six);
    RUN_TEST(test_the_text_layout_is_one_scrolling_row_whatever_the_style_says);
    RUN_TEST(test_a_new_track_starts_the_marquee_at_its_first_character);
    RUN_TEST(test_prefold_maps_apostrophes_and_drops_emoji);
    RUN_TEST(test_the_splash_is_full_width_even_when_the_screen_is_eight_columns);
    RUN_TEST(test_the_splash_gives_a_long_title_two_rows_and_moves_the_artist_down);
    RUN_TEST(test_the_splash_does_not_repeat_a_title_as_its_album);
    RUN_TEST(test_invalidate_forces_a_repaint_of_rows_that_did_not_change);
    RUN_TEST(test_flow_is_one_of_the_styles_by_name);
    RUN_TEST(test_the_tape_reels_turn_only_while_playing);
    RUN_TEST(test_the_tape_fits_eight_columns_and_needs_no_position);
    RUN_TEST(test_the_typewriter_types_a_new_title_then_scrolls_from_the_start);
    RUN_TEST(test_the_typewriter_without_a_track_change_shows_the_title);
    return UNITY_END();
}
