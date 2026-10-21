// The head unit as a now-playing source, and the popup's translation of a key.
//
// Both are here for the same reason: they are the two pieces of this feature
// that can be wrong in a way that LOOKS right on a dashboard. A learned channel
// that latches onto a status frame puts a status frame's bytes on the glass as
// if they were a song; a popup that re-derives the UART code names a byte that
// nothing transmitted. Neither fails, and neither is visible without a car.
#include <unity.h>
#include <string.h>
#include <stdio.h>

#include "media/HuMediaSource.h"
#include "sinks/CanboxUartSink.h"
#include "proto/canbox/Rav4Profile.h"
#include "keys/KeyAction.h"

void setUp() {}
void tearDown() {}

static void feedText(HuMediaSource& s, uint8_t cmd, const char* text, uint32_t now)
{
    s.onFrame(cmd, reinterpret_cast<const uint8_t*>(text), (uint8_t)strlen(text), now);
}

// ---- what counts as text ---------------------------------------------------

static void test_a_status_frame_is_not_text()
{
    // Two flags and a counter. Nothing about this should reach a screen.
    const uint8_t status[] = {0x01, 0x00, 0x07, 0x00};
    TEST_ASSERT_FALSE(HuMediaSource::looksLikeText(status, sizeof(status)));
}

static void test_a_title_with_a_binary_prefix_is_still_text()
{
    // HALF PRINTABLE, NOT ALL. A real title arrives behind a length byte or a
    // codepage marker often enough that demanding 100% would reject the thing
    // we are looking for.
    const uint8_t framed[] = {0x02, 0x0A, 'B', 'o', 'h', 'e', 'm', 'i', 'a', 'n'};
    TEST_ASSERT_TRUE(HuMediaSource::looksLikeText(framed, sizeof(framed)));
}

static void test_too_short_is_never_text()
{
    const uint8_t two[] = {'O', 'K'};
    TEST_ASSERT_FALSE(HuMediaSource::looksLikeText(two, sizeof(two)));
}

// ---- learning which command carries what -----------------------------------

static void test_the_first_text_command_becomes_the_title()
{
    HuMediaSource s;
    feedText(s, 0xC0, "Bohemian Rhapsody", 1000);
    TEST_ASSERT_EQUAL_UINT8(0xC0, s.learnedTitleCmd());
    TEST_ASSERT_EQUAL_STRING("Bohemian Rhapsody", s.current().title.c_str());
}

static void test_the_next_different_command_becomes_the_artist()
{
    HuMediaSource s;
    feedText(s, 0xC0, "Bohemian Rhapsody", 1000);
    feedText(s, 0xC1, "Queen", 1010);
    TEST_ASSERT_EQUAL_UINT8(0xC1, s.learnedArtistCmd());
    TEST_ASSERT_EQUAL_STRING("Bohemian Rhapsody", s.current().title.c_str());
    TEST_ASSERT_EQUAL_STRING("Queen", s.current().artist.c_str());
}

// THE ONE THAT MATTERS. A channel that re-decided itself per frame would swap
// the two rows every time the unit sent a status message that looked like text.
static void test_a_learned_channel_is_sticky()
{
    HuMediaSource s;
    feedText(s, 0xC0, "First Song", 1000);
    feedText(s, 0xC1, "First Artist", 1010);
    feedText(s, 0xC0, "Second Song", 2000);
    feedText(s, 0xC1, "Second Artist", 2010);
    TEST_ASSERT_EQUAL_STRING("Second Song", s.current().title.c_str());
    TEST_ASSERT_EQUAL_STRING("Second Artist", s.current().artist.c_str());
}

static void test_a_third_command_is_ignored_once_both_are_learned()
{
    HuMediaSource s;
    feedText(s, 0xC0, "Song", 1000);
    feedText(s, 0xC1, "Artist", 1010);
    feedText(s, 0xC2, "SOME OTHER TEXT", 1020);
    TEST_ASSERT_EQUAL_STRING("Song", s.current().title.c_str());
    TEST_ASSERT_EQUAL_STRING("Artist", s.current().artist.c_str());
}

// ---- pinning ---------------------------------------------------------------

static void test_a_pinned_title_command_filters_everything_else()
{
    HuMediaSource s;
    s.setChannels(0xC3, 0xC4);
    feedText(s, 0xC0, "NOT THE TITLE", 1000);
    TEST_ASSERT_EQUAL_STRING("", s.current().title.c_str());
    feedText(s, 0xC3, "The Title", 1010);
    TEST_ASSERT_EQUAL_STRING("The Title", s.current().title.c_str());
    feedText(s, 0xC4, "The Artist", 1020);
    TEST_ASSERT_EQUAL_STRING("The Artist", s.current().artist.c_str());
}

// Re-pinning must not leave the previous guess on the screen: what was learned
// under one assignment is not evidence about another.
static void test_changing_the_pin_forgets_what_was_learned()
{
    HuMediaSource s;
    feedText(s, 0xC0, "Guessed Title", 1000);
    TEST_ASSERT_EQUAL_STRING("Guessed Title", s.current().title.c_str());
    s.setChannels(0xC3, 0);
    TEST_ASSERT_EQUAL_STRING("", s.current().title.c_str());
    TEST_ASSERT_EQUAL_UINT8(0, s.learnedTitleCmd());
}

// ---- one channel carrying both ---------------------------------------------

static void test_a_single_channel_splits_artist_and_title()
{
    HuMediaSource s;
    feedText(s, 0xC0, "Queen - Bohemian Rhapsody", 1000);
    TEST_ASSERT_EQUAL_STRING("Queen", s.current().artist.c_str());
    TEST_ASSERT_EQUAL_STRING("Bohemian Rhapsody", s.current().title.c_str());
}

static void test_the_split_order_is_switchable()
{
    HuMediaSource s;
    s.setTitleFirst(true);
    feedText(s, 0xC0, "Bohemian Rhapsody - Queen", 1000);
    TEST_ASSERT_EQUAL_STRING("Bohemian Rhapsody", s.current().title.c_str());
    TEST_ASSERT_EQUAL_STRING("Queen", s.current().artist.c_str());
}

// Once there IS an artist channel, a hyphen in a title is a hyphen in a title.
static void test_a_hyphen_is_not_split_when_two_channels_exist()
{
    HuMediaSource s;
    feedText(s, 0xC0, "Song One", 1000);
    feedText(s, 0xC1, "Artist", 1010);
    feedText(s, 0xC0, "Sun - Moon", 2000);
    TEST_ASSERT_EQUAL_STRING("Sun - Moon", s.current().title.c_str());
    TEST_ASSERT_EQUAL_STRING("Artist", s.current().artist.c_str());
}

// ---- extraction ------------------------------------------------------------

static void test_non_printable_bytes_are_stripped_and_padding_trimmed()
{
    HuMediaSource s;
    const uint8_t framed[] = {0x02, ' ', ' ', 'Q', 'u', 'e', 'e', 'n', ' ', ' ', 0x00};
    s.onFrame(0xC0, framed, sizeof(framed), 1000);
    TEST_ASSERT_EQUAL_STRING("Queen", s.current().title.c_str());
}

// ---- liveness --------------------------------------------------------------

static void test_a_source_that_never_spoke_is_not_active()
{
    HuMediaSource s;
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_EQUAL_STRING("NO HEAD UNIT", s.statusText());
}

// A unit that is talking but has sent no text is a DIFFERENT state from a dead
// wire, and the screen should say so rather than claim there is no head unit.
static void test_frames_without_text_report_a_live_link_with_nothing_to_say()
{
    HuMediaSource s;
    const uint8_t status[] = {0x01, 0x00, 0x07, 0x00};
    s.onFrame(0x84, status, sizeof(status), 1000);
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_EQUAL_STRING("HU: NO TEXT YET", s.statusText());
}

// LIVENESS IS MEASURED FROM ANY FRAME, not from the last text: a unit playing
// one long track sends no text for five minutes and is perfectly alive.
static void test_status_frames_keep_the_link_alive_between_tracks()
{
    HuMediaSource s;
    feedText(s, 0xC0, "A Long Track", 1000);
    TEST_ASSERT_TRUE(s.active());

    const uint8_t status[] = {0x01, 0x00, 0x07, 0x00};
    for (uint32_t t = 10000; t <= 300000; t += 10000)
        s.onFrame(0x84, status, sizeof(status), t);

    s.setNow(305000);
    TEST_ASSERT_TRUE(s.active());
    TEST_ASSERT_EQUAL_STRING("A Long Track", s.current().title.c_str());
}

static void test_silence_expires_the_link()
{
    HuMediaSource s;
    TEST_ASSERT_FALSE(s.heard(5));   // nothing yet: the keys' UART dest is not live
    feedText(s, 0xC0, "Track", 1000);
    TEST_ASSERT_TRUE(s.active());
    s.setNow(1000 + 61000);          // one 0x9D clock period: still alive
    TEST_ASSERT_TRUE(s.active());
    TEST_ASSERT_TRUE(s.heard(1000 + 61000));
    s.setNow(1000 + 151000);         // past the 150 s default
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_FALSE(s.heard(1000 + 151000));
    TEST_ASSERT_EQUAL_STRING("HEAD UNIT QUIET", s.statusText());
}

// ---- the popup's translation ------------------------------------------------
// explain() is what the key popup prints. It must agree with onKey() in every
// case, because the whole reason it exists is that a second derivation would
// silently drift from the one that actually sends.

static uint32_t g_sent = 0;
static uint8_t  g_lastCode = 0, g_lastState = 0;
static void captureFrame(const uint8_t* f, uint8_t n, void*)
{
    // 2E 20 02 <code> <state> <cs>
    if (n >= 6) { g_lastCode = f[3]; g_lastState = f[4]; }
    ++g_sent;
}

static Keys::Event ev(Keys::Action a, Keys::State st, uint8_t code = 0)
{
    Keys::Event e;
    e.action = a;
    e.state = st;
    e.codeHint = code;
    e.tMs = 1000;
    return e;
}

static void test_explain_names_the_byte_that_is_actually_sent()
{
    Sinks::CanboxUartSink sink;
    sink.begin(captureFrame, nullptr);
    g_sent = 0;

    const Keys::Event e = ev(Keys::Action::VolUp, Keys::State::Press);
    const Sinks::CanboxUartSink::Explain x = sink.explain(e);
    sink.onKey(e);

    TEST_ASSERT_TRUE(x.sends);
    TEST_ASSERT_EQUAL_UINT32(1, g_sent);
    TEST_ASSERT_EQUAL_UINT8(g_lastCode, x.code);
    TEST_ASSERT_EQUAL_UINT8(g_lastState, x.state);
}

static void test_explain_reports_a_class_that_is_not_routed()
{
    Sinks::CanboxUartSink sink;
    sink.begin(captureFrame, nullptr);
    sink.setClasses(Keys::CLASS_TRANSPORT);       // volume deliberately excluded
    g_sent = 0;

    const Keys::Event e = ev(Keys::Action::VolUp, Keys::State::Press);
    const Sinks::CanboxUartSink::Explain x = sink.explain(e);
    sink.onKey(e);

    TEST_ASSERT_FALSE(x.sends);
    TEST_ASSERT_EQUAL_UINT32(0, g_sent);
    TEST_ASSERT_EQUAL_STRING("not routed", x.why);
}

static void test_explain_reports_the_link_being_off()
{
    Sinks::CanboxUartSink sink;
    sink.begin(captureFrame, nullptr);
    sink.setEnabled(false);
    g_sent = 0;

    const Keys::Event e = ev(Keys::Action::VolUp, Keys::State::Press);
    const Sinks::CanboxUartSink::Explain x = sink.explain(e);
    sink.onKey(e);

    TEST_ASSERT_FALSE(x.sends);
    TEST_ASSERT_EQUAL_UINT32(0, g_sent);
    TEST_ASSERT_EQUAL_STRING("uart off", x.why);
}

// The wheel's second byte is a DETENT COUNT, not a state, and the popup has to
// say so — "0x85 x1" rather than "0x85 PRESS", which would be a lie about the
// protocol as well as about the byte.
static void test_explain_marks_a_wheel_detent_as_a_count()
{
    Sinks::CanboxUartSink sink;
    sink.begin(captureFrame, nullptr);
    g_sent = 0;

    const Keys::Event e = ev(Keys::Action::EncCw, Keys::State::Press);
    const Sinks::CanboxUartSink::Explain x = sink.explain(e);
    sink.onKey(e);

    TEST_ASSERT_TRUE(x.sends);
    TEST_ASSERT_TRUE(x.knob);
    TEST_ASSERT_FALSE(x.release);           // a rotary has no release to schedule
    TEST_ASSERT_EQUAL_UINT8(Canbox::RAV4_KNOB_TUNE_UP, x.code);
    TEST_ASSERT_EQUAL_UINT8(1, x.state);
    TEST_ASSERT_EQUAL_UINT32(1, g_sent);
    TEST_ASSERT_EQUAL_UINT8(1, g_lastState);
}

// Mute is a LONG press of SOURCE on this profile, not a code of its own — one
// of the four rules that would have had to exist twice.
static void test_explain_knows_mute_is_a_long_source()
{
    Sinks::CanboxUartSink sink;
    sink.begin(captureFrame, nullptr);
    g_sent = 0;

    const Keys::Event e = ev(Keys::Action::Mute, Keys::State::Press);
    const Sinks::CanboxUartSink::Explain x = sink.explain(e);
    sink.onKey(e);

    TEST_ASSERT_TRUE(x.sends);
    TEST_ASSERT_EQUAL_UINT8(Canbox::RAV4_SOURCE, x.code);
    TEST_ASSERT_EQUAL_UINT8(Canbox::KEY_LONG, x.state);
    TEST_ASSERT_EQUAL_UINT8(g_lastCode, x.code);
    TEST_ASSERT_EQUAL_UINT8(g_lastState, x.state);
}

static void test_a_code_override_beats_the_profile_default()
{
    Sinks::CanboxUartSink sink;
    sink.begin(captureFrame, nullptr);
    g_sent = 0;

    const Keys::Event e = ev(Keys::Action::VolUp, Keys::State::Press, 0x42);
    const Sinks::CanboxUartSink::Explain x = sink.explain(e);
    sink.onKey(e);

    TEST_ASSERT_EQUAL_UINT8(0x42, x.code);
    TEST_ASSERT_EQUAL_UINT8(0x42, g_lastCode);
}

// explain() is const and must stay side-effect free: asking what a key means
// may not press it.
static void test_explain_transmits_nothing()
{
    Sinks::CanboxUartSink sink;
    sink.begin(captureFrame, nullptr);
    g_sent = 0;
    for (int i = 0; i < 10; ++i)
        (void)sink.explain(ev(Keys::Action::VolUp, Keys::State::Press));
    TEST_ASSERT_EQUAL_UINT32(0, g_sent);
}

// ---- the structured 0x99 form, from a real DUDU7 on 2026-08-12 -------------
//
// Every vector below is bytes this project actually captured, not bytes it
// invented. That matters more here than usual: the whole message was sitting in
// a log for a session reading ".L.o.s.t. .C.o.n.t.r.o.l" and looking like
// corruption, because it is UTF-16 and everything upstream assumed ASCII.

// `83 02` + "Lost Control" in UTF-16LE, NUL-padded to the fixed 32-byte field.
static const uint8_t kTitleFrame[34] = {
    0x83,0x02, 'L',0,'o',0,'s',0,'t',0,' ',0,'C',0,'o',0,'n',0,
    't',0,'r',0,'o',0,'l',0, 0,0, 0,0, 0,0, 0,0
};
static const uint8_t kArtistFrame[34] = {
    0x83,0x03, 'B',0,'A',0,'L',0,'Z',0,'A',0,'M',0, 0,0, 0,0,
    0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0
};

static void test_the_title_field_decodes_from_utf16()
{
    char out[HuMediaSource::TEXT_MAX];
    TEST_ASSERT_TRUE(HuMediaSource::decodeWide(kTitleFrame + 2, 32, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Lost Control", out);
}

static void test_the_fixed_width_padding_is_trimmed()
{
    char out[HuMediaSource::TEXT_MAX];
    TEST_ASSERT_TRUE(HuMediaSource::decodeWide(kArtistFrame + 2, 32, out, sizeof(out)));
    // Six characters out of a 32-byte field: everything after is padding, and
    // none of it may reach the glass.
    TEST_ASSERT_EQUAL_STRING("BALZAM", out);
}

// The bug this whole change exists to fix, stated as a test: the half-printable
// rule that keeps status frames off the panel throws the track title away with
// them, because UTF-16 is half NUL by construction.
static void test_the_structured_frame_would_fail_the_ascii_heuristic()
{
    TEST_ASSERT_FALSE(HuMediaSource::looksLikeText(kTitleFrame, 34));
}

static void test_both_fields_land_in_the_right_rows()
{
    HuMediaSource hu;
    hu.onFrame(0x99, kTitleFrame,  34, 1000);
    hu.onFrame(0x99, kArtistFrame, 34, 1010);
    TEST_ASSERT_EQUAL_STRING("Lost Control", hu.current().title.c_str());
    TEST_ASSERT_EQUAL_STRING("BALZAM",       hu.current().artist.c_str());
    TEST_ASSERT_TRUE(hu.active());
}

// "Wieże Fabryk" — U+017C is outside Latin-1 entirely, so a byte-wise reader
// cannot represent it at all. Folded to its base letter rather than dropped:
// "Wiee Fabryk" is a different word.
static void test_latin_extended_folds_to_its_base_letter()
{
    const uint8_t p[] = { 'W',0,'i',0,'e',0, 0x7C,0x01, 'e',0,' ',0,
                          'F',0,'a',0,'b',0,'r',0,'y',0,'k',0 };
    char out[HuMediaSource::TEXT_MAX];
    TEST_ASSERT_TRUE(HuMediaSource::decodeWide(p, (uint8_t)sizeof(p), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Wieze Fabryk", out);
}

// Cyrillic was folded to '?' (2026-10-19, in the car): it must come out as
// UTF-8, for the web as is and for the panel's transliteration.
static void test_cyrillic_decodes_to_utf8()
{
    const uint8_t p[] = { 0x1F,0x04, 0x40,0x04, 0x38,0x04, ' ',0, '1',0 };   // "При 1"
    char out[HuMediaSource::TEXT_MAX];
    TEST_ASSERT_TRUE(HuMediaSource::decodeWide(p, (uint8_t)sizeof(p), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("\xD0\x9F\xD1\x80\xD0\xB8 1", out);
}

// A full 15-character Cyrillic field is 30 bytes of UTF-8; a buffer too small
// for it cuts between characters, never inside one.
static void test_a_short_buffer_never_splits_a_character()
{
    const uint8_t p[] = { 0x1F,0x04, 0x40,0x04, 0x38,0x04 };
    char out[6];                      // room for two 2-byte characters + NUL
    TEST_ASSERT_TRUE(HuMediaSource::decodeWide(p, (uint8_t)sizeof(p), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("\xD0\x9F\xD1\x80", out);
}

static void test_the_album_field_lands_in_the_album()
{
    HuMediaSource hu;
    uint8_t f[34] = { 0x83, 0x04, 'L',0,'P',0 };
    hu.onFrame(0x99, kTitleFrame, 34, 1000);
    hu.onFrame(0x99, f, 34, 1010);
    TEST_ASSERT_EQUAL_STRING("LP", hu.current().album.c_str());
    TEST_ASSERT_EQUAL_STRING("Lost Control", hu.current().title.c_str());
}

// Field 01 is the track number and total, binary (k0.a3.c3): it must never
// land in a text row, nor blank one. Track 65 of 66 would read "A", "B".
static void test_the_track_number_field_is_not_text()
{
    HuMediaSource hu;
    hu.onFrame(0x99, kTitleFrame,  34, 1000);
    uint8_t f[34] = { 0x83, 0x01, 0x41, 0x00, 0x42, 0x00 };
    hu.onFrame(0x99, f, 34, 1020);
    TEST_ASSERT_EQUAL_STRING("Lost Control", hu.current().title.c_str());
    TEST_ASSERT_EQUAL_STRING("", hu.current().artist.c_str());
}

// The head unit sends this on an automatic track change, before the two text
// frames. It carries no text and must not blank a row.
static void test_the_bare_marker_frame_changes_nothing()
{
    HuMediaSource hu;
    hu.onFrame(0x99, kTitleFrame, 34, 1000);
    uint8_t empty[34] = { 0x20 };
    hu.onFrame(0x99, empty, 34, 1100);
    TEST_ASSERT_EQUAL_STRING("Lost Control", hu.current().title.c_str());
}

static void test_a_payload_that_is_not_whole_code_units_is_refused()
{
    const uint8_t odd[] = { 'A', 0, 'B' };
    char out[HuMediaSource::TEXT_MAX];
    TEST_ASSERT_FALSE(HuMediaSource::decodeWide(odd, (uint8_t)sizeof(odd), out, sizeof(out)));
    // An all-padding field is "no text", not an empty title that would blank the row.
    const uint8_t pad[8] = {0};
    TEST_ASSERT_FALSE(HuMediaSource::decodeWide(pad, (uint8_t)sizeof(pad), out, sizeof(out)));
}

static void test_a_pinned_channel_still_filters_the_structured_form()
{
    HuMediaSource hu;
    hu.setChannels(0x99, 0);
    hu.onFrame(0x77, kTitleFrame, 34, 1000);   // wrong command, must be ignored
    TEST_ASSERT_EQUAL_STRING("", hu.current().title.c_str());
    hu.onFrame(0x99, kTitleFrame, 34, 1010);
    TEST_ASSERT_EQUAL_STRING("Lost Control", hu.current().title.c_str());
}

// ---- the snapshot (Step 2) -----------------------------------------------------
// The hu task writes; the panel's media screen and httpd read on other tasks.
// Strings cross only as a copy taken under the source's mutex.

static void test_a_snapshot_is_a_copy_of_what_was_parsed()
{
    HuMediaSource s;
    feedText(s, 0xC0, "Bohemian Rhapsody", 1000);
    feedText(s, 0xC1, "Queen", 1010);
    const MediaInfo m = s.snapshot();
    TEST_ASSERT_EQUAL_STRING("Bohemian Rhapsody", m.title.c_str());
    TEST_ASSERT_EQUAL_STRING("Queen", m.artist.c_str());
    // A copy: the next track does not change it.
    feedText(s, 0xC0, "Another One", 2000);
    TEST_ASSERT_EQUAL_STRING("Bohemian Rhapsody", m.title.c_str());
    TEST_ASSERT_EQUAL_STRING("Another One", s.snapshot().title.c_str());
    TEST_ASSERT_EQUAL(0u, s.lockMisses());
}

// The change callback runs after the mutex is released, so a reader that takes
// a snapshot from inside it gets the new title, not an empty copy.
static HuMediaSource* g_src = nullptr;
static char g_seen[32];
static void test_the_change_callback_may_take_a_snapshot()
{
    HuMediaSource s;
    g_src = &s;
    g_seen[0] = 0;
    s.setChangeCallback([](IMediaSource&, void*) {
        snprintf(g_seen, sizeof(g_seen), "%s", g_src->snapshot().title.c_str());
    }, nullptr);
    feedText(s, 0xC0, "Bohemian Rhapsody", 1000);
    TEST_ASSERT_EQUAL_STRING("Bohemian Rhapsody", g_seen);
}

static void test_the_web_setters_still_apply()
{
    HuMediaSource s;
    feedText(s, 0xC0, "Guessed Title", 1000);
    s.setChannels(0xD0, 0);
    TEST_ASSERT_EQUAL_STRING("", s.snapshot().title.c_str());
    s.setTitleFirst(true);
    TEST_ASSERT_TRUE(s.titleFirst());
    feedText(s, 0xD0, "Sun - Moon", 2000);
    s.forget();
    TEST_ASSERT_EQUAL_STRING("", s.snapshot().title.c_str());
}

// The hu task never waits for a reader: a frame that finds the mutex held is
// kept, and applied in order by the next frame or service() that gets it.
static void test_a_frame_that_finds_the_mutex_held_is_applied_later_in_order()
{
    HuMediaSource s;
    TEST_ASSERT_TRUE(s.testHold());                  // a reader mid-copy
    feedText(s, 0xC0, "Bohemian Rhapsody", 1000);
    feedText(s, 0xC1, "Queen", 1010);
    TEST_ASSERT_EQUAL(2u, s.lockMisses());
    TEST_ASSERT_EQUAL(2u, s.frames());               // liveness still counted
    TEST_ASSERT_EQUAL_STRING("", s.current().title.c_str());
    s.service();                                     // still held: nothing yet
    TEST_ASSERT_EQUAL_STRING("", s.current().title.c_str());
    s.testRelease();
    s.service();
    // Learned in arrival order: the first text command is the title.
    TEST_ASSERT_EQUAL_STRING("Bohemian Rhapsody", s.current().title.c_str());
    TEST_ASSERT_EQUAL_STRING("Queen", s.current().artist.c_str());
    TEST_ASSERT_EQUAL(0u, s.stashDropped());
}

// ---- the panel task's read (Step 3) ---------------------------------------------
// The media screen refills on the panel task, which must not wait on a reader
// or the hu task: it asks with a bounded wait, and a busy mutex is a "not now"
// that changes nothing, so the refill simply happens on a later pass.
static void test_a_held_mutex_makes_try_snapshot_say_not_now()
{
    HuMediaSource s;
    feedText(s, 0xC0, "Bohemian Rhapsody", 1000);
    MediaInfo m;
    m.title = "untouched";
    TEST_ASSERT_TRUE(s.testHold());
    TEST_ASSERT_FALSE(s.trySnapshot(m, 5));
    TEST_ASSERT_EQUAL_STRING("untouched", m.title.c_str());
    s.testRelease();
    TEST_ASSERT_TRUE(s.trySnapshot(m, 5));
    TEST_ASSERT_EQUAL_STRING("Bohemian Rhapsody", m.title.c_str());
}

static void test_a_full_stash_keeps_the_newest_and_counts_the_rest()
{
    HuMediaSource s;
    s.setChannels(0xC0, 0);
    TEST_ASSERT_TRUE(s.testHold());
    feedText(s, 0xC0, "Track One", 1000);
    feedText(s, 0xC0, "Track Two", 1010);
    feedText(s, 0xC0, "Track Three", 1020);
    s.testRelease();
    // The next frame applies what was kept first, then itself.
    feedText(s, 0xC0, "Track Four", 1030);
    TEST_ASSERT_EQUAL_STRING("Track Four", s.current().title.c_str());
    TEST_ASSERT_EQUAL(1u, s.stashDropped());
    TEST_ASSERT_EQUAL(4u, s.textFrames() + s.stashDropped());
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_frame_that_finds_the_mutex_held_is_applied_later_in_order);
    RUN_TEST(test_a_full_stash_keeps_the_newest_and_counts_the_rest);
    RUN_TEST(test_a_held_mutex_makes_try_snapshot_say_not_now);
    RUN_TEST(test_a_snapshot_is_a_copy_of_what_was_parsed);
    RUN_TEST(test_the_change_callback_may_take_a_snapshot);
    RUN_TEST(test_the_web_setters_still_apply);
    RUN_TEST(test_a_status_frame_is_not_text);
    RUN_TEST(test_a_title_with_a_binary_prefix_is_still_text);
    RUN_TEST(test_too_short_is_never_text);
    RUN_TEST(test_the_first_text_command_becomes_the_title);
    RUN_TEST(test_the_next_different_command_becomes_the_artist);
    RUN_TEST(test_a_learned_channel_is_sticky);
    RUN_TEST(test_a_third_command_is_ignored_once_both_are_learned);
    RUN_TEST(test_a_pinned_title_command_filters_everything_else);
    RUN_TEST(test_changing_the_pin_forgets_what_was_learned);
    RUN_TEST(test_a_single_channel_splits_artist_and_title);
    RUN_TEST(test_the_split_order_is_switchable);
    RUN_TEST(test_a_hyphen_is_not_split_when_two_channels_exist);
    RUN_TEST(test_non_printable_bytes_are_stripped_and_padding_trimmed);
    RUN_TEST(test_a_source_that_never_spoke_is_not_active);
    RUN_TEST(test_frames_without_text_report_a_live_link_with_nothing_to_say);
    RUN_TEST(test_status_frames_keep_the_link_alive_between_tracks);
    RUN_TEST(test_silence_expires_the_link);
    RUN_TEST(test_explain_names_the_byte_that_is_actually_sent);
    RUN_TEST(test_explain_reports_a_class_that_is_not_routed);
    RUN_TEST(test_explain_reports_the_link_being_off);
    RUN_TEST(test_explain_marks_a_wheel_detent_as_a_count);
    RUN_TEST(test_explain_knows_mute_is_a_long_source);
    RUN_TEST(test_a_code_override_beats_the_profile_default);
    RUN_TEST(test_explain_transmits_nothing);

    RUN_TEST(test_the_title_field_decodes_from_utf16);
    RUN_TEST(test_the_fixed_width_padding_is_trimmed);
    RUN_TEST(test_the_structured_frame_would_fail_the_ascii_heuristic);
    RUN_TEST(test_both_fields_land_in_the_right_rows);
    RUN_TEST(test_latin_extended_folds_to_its_base_letter);
    RUN_TEST(test_the_bare_marker_frame_changes_nothing);
    RUN_TEST(test_cyrillic_decodes_to_utf8);
    RUN_TEST(test_a_short_buffer_never_splits_a_character);
    RUN_TEST(test_the_album_field_lands_in_the_album);
    RUN_TEST(test_the_track_number_field_is_not_text);
    RUN_TEST(test_a_payload_that_is_not_whole_code_units_is_refused);
    RUN_TEST(test_a_pinned_channel_still_filters_the_structured_form);
    return UNITY_END();
}
