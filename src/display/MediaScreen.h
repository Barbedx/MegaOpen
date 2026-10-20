#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../utils/UiList.h"

// The now-playing screen as three rows of ASCII. Pure (no Arduino), host-tested.
//
// Layout, at the default 26-column row:
//
//     [12:34] > 1:23/3:45 [##--]    row 1: clock, transport, position, progress
//     Bohemian Rhapsody             row 2: title,  marquee if it does not fit
//     Queen                         row 3: artist, marquee if it does not fit
//
// Clock is leftmost and fixed-width; omitted until a time is set.
// Glyphs are ASCII: the panel's charset is a Latin subset and renders anything
// else as garbage.
namespace MegaOpen
{
    class MediaScreen {
    public:
        enum class State : uint8_t { Paused = 0, Playing, Rewinding, Forwarding };

        // Which panel primitive carries the rows (runtime switch):
        //   Menu        showMenu, two rows + header. Full screen, covers the pane.
        //   Fullscreen  showFullscreenText. Covers the pane (a pane image under
        //               it still costs 44 CAN frames).
        //   Info        showInfoMenu, three rows beside the 48x48 pane. Three
        //               separate messages: the heaviest on the wire.
        //   Text        setText, one 8-char row beside the pane; the panel draws
        //               its own clock. Cheapest (one message). Style is ignored,
        //               compose() draws a one-row marquee.
        enum class Layout : uint8_t { Menu = 0, Fullscreen, Info, Text };
        static constexpr uint8_t LAYOUT_COUNT = 4;

        static bool sharesWithPane(Layout l) { return l == Layout::Info || l == Layout::Text; }
        static uint8_t rowsFor(Layout l) { return l == Layout::Text ? 1 : 3; }
        // setText: the panel draws its own clock beside the row.
        static bool panelClock(Layout l) { return l == Layout::Text; }

        static const char* layoutName(Layout l);
        static Layout layoutFromName(const char* s);

        // One row per layout for /api/catalog: id, Ukrainian name, and the
        // settings it reads as the page's form fields.
        struct LayoutRow {
            Layout         layout;
            const char*    id;
            const char*    name;
            const UiField* params;
            uint8_t        nParams;
        };
        static const LayoutRow& layoutAt(uint8_t i);    // i < LAYOUT_COUNT

        // ---- styles: runtime-switchable, each a pure fn of (content, geometry, nowMs)
        // Animated styles repaint on their own (~14 frames each); the panel
        // sustains ~5 screens/s, and main.cpp's repaint gate limits them.
        enum class Style : uint8_t {
            Classic = 0,   // [12:34] > 1:23/3:45 [##--] / title / artist
            Minimal,       // clock only, then title and artist. Nothing that moves.
            Bar,           // title / artist / a full-width progress bar and position
            BigTitle,      // title wrapped across rows 1-2, artist on 3
            Ticker,        // one continuous band scrolling through all three rows
            Split,         // artist above title, position without a bar
            Pulse,         // animated: a sweep that runs with the transport
            Scanner,       // animated: a cell scanning back and forth
            Clock,         // the time, spaced out and centred, over title/artist
            Vu,            // animated: a level meter driven by the position
            // Classic's header; title wraps over rows 2-3 instead of scrolling,
            // artist/album on a second page.
            Flow,
            // Title, artist, album; nothing else. For the head unit, which
            // sends no position and no play state.
            Plain,
            // Winamp: clock + a dancing spectrum, then "Artist - Title *** "
            // looping, then the album.
            Winamp,
            _Count
        };
        static constexpr uint8_t STYLE_COUNT = (uint8_t)Style::_Count;

        void setStyle(Style s) { _style = (uint8_t)s < STYLE_COUNT ? s : Style::Classic; }
        Style style() const { return _style; }

        // The name, not the number, is stored (web UI, NVS).
        static const char* styleName(Style s);
        static const char* styleTitle(Style s);      // Ukrainian, for /api/catalog
        static Style styleFromName(const char* s);
        static bool styleAnimates(Style s);

        static constexpr uint8_t ROW_MAX = 32;      // buffer size, including NUL
        static constexpr uint8_t COLS_DEFAULT = 26; // usable Carminat window row

        void setTitle(const char* s);
        void setArtist(const char* s);
        // Only Flow draws it; often empty (the head unit never sends one).
        void setAlbum(const char* s);
        const char* album() const { return _album; }

        // Flow page durations; every flip is a ~14-frame repaint.
        static constexpr uint32_t FLOW_TITLE_MS = 6000;
        static constexpr uint32_t FLOW_INFO_MS  = 3000;

        // "1234" or "12:34"; null, empty or anything not four digits clears it.
        void setClock(const char* hhmm);
        const char* clock() const { return _clock; }

        void setState(State s) { _state = s; }
        void setProgress(uint16_t elapsedSec, uint16_t durationSec) {
            _elapsed = elapsedSec; _duration = durationSec;
        }

        // Whether the source reports play state and position. AMS does; the Raise
        // UART head unit does not, so with false row 1 is the clock alone instead
        // of a wrong `|| 0:00`.
        void setHasTransport(bool on) { _hasTransport = on; }
        bool hasTransport() const { return _hasTransport; }

        void setCols(uint8_t c) { _cols = (c < 8) ? 8 : (c > ROW_MAX - 1 ? ROW_MAX - 1 : c); }
        uint8_t cols() const { return _cols; }
        void setBarWidth(uint8_t w) { _bar = (w > 20) ? 20 : w; }
        uint8_t barWidth() const { return _bar; }

        // ms per marquee step, per row; 0 freezes. Separate rates so title and
        // artist drift apart instead of scrolling in lockstep.
        void setScrollMsTitle(uint16_t ms)  { _scrollMsTitle = ms; }
        void setScrollMsArtist(uint16_t ms) { _scrollMsArtist = ms; }
        uint16_t scrollMsTitle() const  { return _scrollMsTitle; }
        uint16_t scrollMsArtist() const { return _scrollMsArtist; }

        // Both rows; what the stored setting and older API field mean.
        void setScrollMs(uint16_t ms) { _scrollMsTitle = ms; _scrollMsArtist = ms; }
        // A new track restarts every marquee at its first character and holds it
        // there for MARQUEE_LEAD_MS. Without it the offset runs off the wall
        // clock and a new title first appears as a slice from its middle.
        static constexpr uint32_t MARQUEE_LEAD_MS = 1200;
        void restartMarquee(uint32_t nowMs) { _marqueeT0 = nowMs; _marqueeLead = true; }
        uint16_t scrollMs() const { return _scrollMsTitle; }

        void setLayout(Layout l) { _layout = l; }
        Layout layout() const { return _layout; }

        State state() const { return _state; }
        const char* title() const { return _title; }
        const char* artist() const { return _artist; }
        uint16_t elapsed() const { return _elapsed; }
        uint16_t duration() const { return _duration; }

        // Pure: same inputs and nowMs give the same bytes. Buffers >= ROW_MAX.
        void compose(uint32_t nowMs, char* row1, char* row2, char* row3) const;

        // True when compose() now differs from the last markRendered(); an
        // unchanged repaint still costs 13 frames and an ACK each.
        bool dirty(uint32_t nowMs) const;
        void markRendered(uint32_t nowMs);
        // Force the next dirty(), e.g. after the track splash drew over the glass.
        void invalidate() { _lastRow1[0] = '\x01'; _lastRow1[1] = '\0'; }

        // Measured: beside the 48x48 pane an info row shows 8 chars, silently
        // truncated. setText's field is 8 chars by protocol.
        static constexpr uint8_t COLS_INFO = 8;
        static uint8_t colsFor(Layout l) { return sharesWithPane(l) ? COLS_INFO : COLS_DEFAULT; }

        // Track-change splash: title/artist/album centred, drawn as a fullscreen
        // at full width even when the regular layout is 8 columns. A long title
        // takes two rows and pushes the artist down.
        void composeSplash(char* row1, char* row2, char* row3,
                           uint8_t cols = COLS_DEFAULT) const;

        // Wrap over two rows (at a space if possible, else mid-word). False and
        // nothing written if it does not fit. Used by the notification screen.
        static bool wrapTwo(const char* s, uint8_t cols, char* a, char* b);

    private:
        // 0 when the string fits or stepMs is 0. Derived from nowMs, never
        // accumulated, so skipped repaints build no backlog.
        uint8_t offsetFor(uint8_t len, uint32_t nowMs, uint16_t stepMs) const;
        uint32_t _marqueeT0   = 0;
        bool     _marqueeLead = false;
        static void window(const char* src, uint8_t offset, uint8_t cols, char* out);
        static void mmss(uint16_t sec, char* out);   // out >= 6

        // ---- one per Style, each writing all three rows ----------------------
        void classic(uint32_t now, char* r1, char* r2, char* r3) const;
        void minimal(uint32_t now, char* r1, char* r2, char* r3) const;
        void bar(uint32_t now, char* r1, char* r2, char* r3) const;
        void bigTitle(uint32_t now, char* r1, char* r2, char* r3) const;
        void ticker(uint32_t now, char* r1, char* r2, char* r3) const;
        void split(uint32_t now, char* r1, char* r2, char* r3) const;
        void pulse(uint32_t now, char* r1, char* r2, char* r3) const;
        void scanner(uint32_t now, char* r1, char* r2, char* r3) const;
        void clockBig(uint32_t now, char* r1, char* r2, char* r3) const;
        void vu(uint32_t now, char* r1, char* r2, char* r3) const;
        void flow(uint32_t now, char* r1, char* r2, char* r3) const;
        void plain(uint32_t now, char* r1, char* r2, char* r3) const;
        void winamp(uint32_t now, char* r1, char* r2, char* r3) const;

        // Classic/Flow row 1; the clock alone when the source has no transport.
        void header(char* out) const;
        void titleRow(uint32_t now, char* out) const;   // the marqueed title
        void artistRow(uint32_t now, char* out) const;  // the marqueed artist
        // Layout::Text: "title - artist" as one marquee in row 1.
        void lineRow(uint32_t now, char* out) const;
        const char* glyph() const;                      // > || << >>
        // "1:23/3:45", or "1:23" with no duration, or "" with no transport.
        void position(char* out, size_t cap) const;
        // inner+2 chars plus NUL, never more than cap.
        void progressBar(char* out, size_t cap, uint8_t inner) const;
        static void centre(const char* src, uint8_t cols, char* out);

        char     _title[ROW_MAX * 2] = {0};
        char     _artist[ROW_MAX * 2] = {0};
        char     _album[ROW_MAX * 2] = {0};
        char     _clock[6] = {0};       // "HH:MM", empty until a time is known
        State    _state = State::Paused;
        bool     _hasTransport = true;
        Layout   _layout = Layout::Menu;
        Style    _style = Style::Classic;
        uint16_t _elapsed = 0, _duration = 0;
        uint8_t  _cols = COLS_DEFAULT;
        uint8_t  _bar = 10;
        uint16_t _scrollMsTitle  = 700;
        uint16_t _scrollMsArtist = 1400;   // half speed, so the rows drift apart

        char     _lastRow1[ROW_MAX] = {0};
        char     _lastRow2[ROW_MAX] = {0};
        char     _lastRow3[ROW_MAX] = {0};
    };
}
