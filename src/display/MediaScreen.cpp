#include "MediaScreen.h"
#include <string.h>
#include <stdio.h>

namespace MegaOpen
{
    // Separates the end of a marquee from its own beginning.
    static const char kGap[] = "   -   ";
    static constexpr uint8_t kGapLen = sizeof(kGap) - 1;

    // The last byte of the buffer is never written, so it keeps its initial NUL.
    // Written by the loop task only, but read by the web handler (/api/media);
    // a racing strlen() can see torn text but can never run past the buffer.
    static void copyBounded(char* dst, size_t cap, const char* src)
    {
        if (cap == 0) return;
        if (!src) { dst[0] = '\0'; return; }
        size_t n = strlen(src);
        if (cap >= 2 && n > cap - 2) n = cap - 2;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }

    void MediaScreen::setTitle(const char* s)  { copyBounded(_title, sizeof(_title), s); }
    void MediaScreen::setArtist(const char* s) { copyBounded(_artist, sizeof(_artist), s); }
    void MediaScreen::setAlbum(const char* s)  { copyBounded(_album, sizeof(_album), s); }

    // Validated rather than copied: anything but four valid digits leaves the
    // clock unset, so a bad time shows no clock instead of a plausible 00:00.
    // Accepts "1234" (firmware) and "12:34" (web console).
    void MediaScreen::setClock(const char* hhmm)
    {
        _clock[0] = '\0';
        if (!hhmm) return;

        char d[5];
        uint8_t n = 0;
        for (const char* p = hhmm; *p && n < 4; ++p) {
            if (*p == ':' || *p == ' ') continue;
            if (*p < '0' || *p > '9') return;      // junk: leave the clock unset
            d[n++] = *p;
        }
        if (n != 4) return;

        const uint8_t h = (uint8_t)((d[0] - '0') * 10 + (d[1] - '0'));
        const uint8_t m = (uint8_t)((d[2] - '0') * 10 + (d[3] - '0'));
        if (h > 23 || m > 59) return;

        _clock[0] = d[0]; _clock[1] = d[1];
        _clock[2] = ':';
        _clock[3] = d[2]; _clock[4] = d[3];
        _clock[5] = '\0';
    }

    void MediaScreen::mmss(uint16_t sec, char* out)
    {
        const uint16_t m = sec / 60;
        const uint16_t s = sec % 60;
        // Minutes are not clamped: a 100-minute podcast reads 100:12.
        snprintf(out, 8, "%u:%02u", (unsigned)m, (unsigned)s);
    }

    uint8_t MediaScreen::offsetFor(uint8_t len, uint32_t nowMs, uint16_t stepMs) const
    {
        if (len <= _cols || stepMs == 0) return 0;
        const uint16_t period = (uint16_t)(len + kGapLen);
        uint32_t t = nowMs - _marqueeT0;
        if (_marqueeLead) {
            if (t < MARQUEE_LEAD_MS) return 0;
            t -= MARQUEE_LEAD_MS;
        }
        return (uint8_t)((t / stepMs) % period);
    }

    // One window of `cols` characters over src, wrapping through the gap, without
    // building a doubled copy of the string.
    void MediaScreen::window(const char* src, uint8_t offset, uint8_t cols, char* out)
    {
        const uint8_t len = (uint8_t)strlen(src);
        if (len <= cols) {
            memcpy(out, src, len);
            out[len] = '\0';
            return;
        }
        const uint16_t period = (uint16_t)(len + kGapLen);
        for (uint8_t i = 0; i < cols; ++i) {
            const uint16_t p = (uint16_t)((offset + i) % period);
            out[i] = (p < len) ? src[p] : kGap[p - len];
        }
        out[cols] = '\0';
    }

    // ---- the style and layout tables ----------------------------------------
    // NVS and the web UI store ids, not indexes; /api/catalog serves these rows
    // as they are. Order must match the enum.
    static const UiName kStyles[] = {
        {"classic",  "класичний"},
        {"minimal",  "мінімал"},
        {"bar",      "смуга"},
        {"bigtitle", "велика назва"},
        {"ticker",   "тікер"},
        {"split",    "виконавець зверху"},
        {"pulse",    "пульс"},
        {"scanner",  "сканер"},
        {"clock",    "великий годинник"},
        {"vu",       "рівень"},
        {"flow",     "потік"},
        {"plain",    "просто текст"},
        {"winamp",   "вінамп"},
    };
    static_assert(sizeof(kStyles) / sizeof(kStyles[0]) == MediaScreen::STYLE_COUNT,
                  "style names and the Style enum have drifted apart");

    // The settings each layout reads, as the page's form. Text draws no style
    // (compose() ignores it); only Menu carries the scroll-arrow byte.
    static constexpr UiField kStyle  = uiEnum("style", "Стиль", "styles");
    static constexpr UiField kScroll = uiEnum("scroll", "Стрілки прокрутки", "arrows");
    static constexpr UiField kTitle  = uiNum("scrollMsTitle", "Прокрутка назви, мс", 0, 5000, 50, "0 = стоїть");
    static constexpr UiField kArtist = uiNum("scrollMsArtist", "Прокрутка виконавця, мс", 0, 5000, 50);
    static constexpr UiField kSplash = uiNum("splashMs", "Заставка нового треку, мс", 0, 20000, 500,
                                             "0 = вимк. На весь екран, 22 символи");
    static const UiField kTextParams[] = {
        uiNum("scrollMsTitle", "Біжучий рядок, мс на крок", 0, 5000, 50, "0 = стоїть"), kSplash};
    static const UiField kInfoParams[] = {kStyle, kTitle, kArtist, kSplash};
    static const UiField kMenuParams[] = {kStyle, kScroll, kTitle, kArtist};
    static const UiField kFullParams[] = {kStyle, kTitle, kArtist};
    #define PARAMS(a) a, (uint8_t)(sizeof(a) / sizeof(a[0]))

    // In the order the page lists them: cheapest on the wire first.
    static const MediaScreen::LayoutRow kLayouts[] = {
        {MediaScreen::Layout::Text,       "text",       "Текст · 8×1",            PARAMS(kTextParams)},
        {MediaScreen::Layout::Info,       "info",       "Інфо · 8×3 + картинка",  PARAMS(kInfoParams)},
        {MediaScreen::Layout::Menu,       "menu",       "Меню · 26×3",            PARAMS(kMenuParams)},
        {MediaScreen::Layout::Fullscreen, "fullscreen", "Повний екран · 26×3",    PARAMS(kFullParams)},
    };
    #undef PARAMS
    static_assert(sizeof(kLayouts) / sizeof(kLayouts[0]) == MediaScreen::LAYOUT_COUNT,
                  "a Layout has no row in kLayouts");

    const MediaScreen::LayoutRow& MediaScreen::layoutAt(uint8_t i)
    {
        return kLayouts[i < LAYOUT_COUNT ? i : 0];
    }

    const char* MediaScreen::layoutName(Layout l)
    {
        for (const auto& r : kLayouts) if (r.layout == l) return r.id;
        return "fullscreen";
    }

    MediaScreen::Layout MediaScreen::layoutFromName(const char* s)
    {
        if (s) for (const auto& r : kLayouts) if (!strcmp(s, r.id)) return r.layout;
        return Layout::Fullscreen;
    }

    const char* MediaScreen::styleName(Style s)
    {
        const uint8_t i = (uint8_t)s;
        return i < STYLE_COUNT ? kStyles[i].id : "classic";
    }

    const char* MediaScreen::styleTitle(Style s)
    {
        const uint8_t i = (uint8_t)s;
        return i < STYLE_COUNT ? kStyles[i].name : "";
    }

    MediaScreen::Style MediaScreen::styleFromName(const char* s)
    {
        if (!s) return Style::Classic;
        for (uint8_t i = 0; i < STYLE_COUNT; ++i)
            if (!strcmp(s, kStyles[i].id)) return (Style)i;
        return Style::Classic;
    }

    bool MediaScreen::styleAnimates(Style s)
    {
        // Ticker's band moves even when the strings fit a row; Winamp's
        // spectrum dances whenever a title is up.
        return s == Style::Pulse || s == Style::Scanner ||
               s == Style::Vu    || s == Style::Ticker || s == Style::Winamp;
    }

    // ---- shared pieces ------------------------------------------------------

    const char* MediaScreen::glyph() const
    {
        switch (_state) {
            case State::Playing:    return ">";
            case State::Rewinding:  return "<<";
            case State::Forwarding: return ">>";
            default:                return "||";
        }
    }

    void MediaScreen::titleRow(uint32_t now, char* out) const
    {
        window(_title, offsetFor((uint8_t)strlen(_title), now, _scrollMsTitle), _cols, out);
    }

    void MediaScreen::artistRow(uint32_t now, char* out) const
    {
        window(_artist, offsetFor((uint8_t)strlen(_artist), now, _scrollMsArtist), _cols, out);
    }

    void MediaScreen::position(char* out, size_t cap) const
    {
        if (!_hasTransport) { out[0] = '\0'; return; }
        char el[8], du[8];
        mmss(_elapsed, el);
        if (_duration > 0) {
            mmss(_duration, du);
            snprintf(out, cap, "%s/%s", el, du);
        } else {
            snprintf(out, cap, "%s", el);
        }
    }

    void MediaScreen::progressBar(char* out, size_t cap, uint8_t inner) const
    {
        if (cap < 3u + inner) { out[0] = '\0'; return; }
        uint8_t filled = 0;
        if (_duration > 0 && inner) {
            const uint32_t f = (uint32_t)_elapsed * (uint32_t)inner / _duration;
            filled = (uint8_t)(f > (uint32_t)inner ? (uint32_t)inner : f);
        }
        size_t n = 0;
        out[n++] = '[';
        for (uint8_t i = 0; i < inner; ++i) out[n++] = (i < filled) ? '#' : '-';
        out[n++] = ']';
        out[n] = '\0';
    }

    // Pad `src` to the middle of `cols`. Trailing spaces are kept, or the row
    // drifts left on this panel.
    void MediaScreen::centre(const char* src, uint8_t cols, char* out)
    {
        const uint8_t len = (uint8_t)strlen(src);
        if (len >= cols) { memcpy(out, src, cols); out[cols] = '\0'; return; }
        const uint8_t pad = (uint8_t)((cols - len) / 2);
        memset(out, ' ', pad);
        memcpy(out + pad, src, len);
        const uint8_t rest = (uint8_t)(cols - pad - len);
        memset(out + pad + len, ' ', rest);
        out[cols] = '\0';
    }

    // ---- the dispatcher -----------------------------------------------------

    void MediaScreen::lineRow(uint32_t nowMs, char* out) const
    {
        // Title and artist as one band, joined by the marquee gap. No position
        // or glyph; the panel draws the clock itself.
        char band[ROW_MAX * 4];
        if (_artist[0])
            snprintf(band, sizeof(band), "%s%s%s", _title[0] ? _title : "---", kGap, _artist);
        else
            snprintf(band, sizeof(band), "%s", _title[0] ? _title : "---");
        const uint8_t off = offsetFor((uint8_t)strlen(band), nowMs, _scrollMsTitle);
        window(band, off, _cols, out);
    }

    void MediaScreen::compose(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        // The one-row Text layout overrides the style: no style fits one row of
        // eight. Rows 2 and 3 come back empty so dirty() still works.
        if (_layout == Layout::Text) {
            lineRow(nowMs, row1);
            row2[0] = '\0';
            row3[0] = '\0';
            return;
        }
        switch (_style) {
            case Style::Minimal:  minimal(nowMs, row1, row2, row3);  return;
            case Style::Bar:      bar(nowMs, row1, row2, row3);      return;
            case Style::BigTitle: bigTitle(nowMs, row1, row2, row3); return;
            case Style::Ticker:   ticker(nowMs, row1, row2, row3);   return;
            case Style::Split:    split(nowMs, row1, row2, row3);    return;
            case Style::Pulse:    pulse(nowMs, row1, row2, row3);    return;
            case Style::Scanner:  scanner(nowMs, row1, row2, row3);  return;
            case Style::Clock:    clockBig(nowMs, row1, row2, row3); return;
            case Style::Vu:       vu(nowMs, row1, row2, row3);       return;
            case Style::Flow:     flow(nowMs, row1, row2, row3);     return;
            case Style::Plain:    plain(nowMs, row1, row2, row3);    return;
            case Style::Winamp:   winamp(nowMs, row1, row2, row3);   return;
            default:              classic(nowMs, row1, row2, row3);  return;
        }
    }

    // ---- 0. classic ---------------------------------------------------------
    // The default: header row with the clock leftmost, then title and artist.
    void MediaScreen::classic(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        header(row1);

        // Different step rates, so equal-length strings do not scroll as one block.
        window(_title,  offsetFor((uint8_t)strlen(_title),  nowMs, _scrollMsTitle),
               _cols, row2);
        window(_artist, offsetFor((uint8_t)strlen(_artist), nowMs, _scrollMsArtist),
               _cols, row3);
    }

    // Classic's row 1, shared with Flow.
    void MediaScreen::header(char* row1) const
    {
        const char* glyph = this->glyph();

        char el[8], du[8];
        mmss(_elapsed, el);
        mmss(_duration, du);

        // Clock first: everything after it is variable width.
        char head[ROW_MAX];
        int n;
        if (!_hasTransport) {
            // Clock only, or an empty row if there is none.
            n = _clock[0] ? snprintf(head, sizeof(head), "[%s]", _clock) : 0;
            if (n < 0) n = 0;
            head[n] = '\0';
            window(head, 0, _cols, row1);
            return;
        }
        if (_clock[0])
            n = (_duration > 0)
                ? snprintf(head, sizeof(head), "[%s] %s %s/%s", _clock, glyph, el, du)
                : snprintf(head, sizeof(head), "[%s] %s %s", _clock, glyph, el);
        else
            n = (_duration > 0)
                ? snprintf(head, sizeof(head), "%s %s/%s", glyph, el, du)
                : snprintf(head, sizeof(head), "%s %s", glyph, el);
        if (n < 0) n = 0;
        if (n > (int)sizeof(head) - 1) n = (int)sizeof(head) - 1;

        // The bar shrinks to fit what is left; below three cells it is dropped.
        const int room  = (int)_cols - n - 1;             // -1 for the space
        int       inner = (int)_bar;
        if (inner > room - 2) inner = room - 2;           // -2 for the brackets
        if (_bar >= 3 && inner >= 3) {
            uint8_t filled = 0;
            if (_duration > 0) {
                uint32_t f = (uint32_t)_elapsed * (uint32_t)inner / _duration;
                filled = (uint8_t)(f > (uint32_t)inner ? (uint32_t)inner : f);
            }
            head[n++] = ' ';
            head[n++] = '[';
            for (int i = 0; i < inner; ++i) head[n++] = (i < filled) ? '#' : '-';
            head[n++] = ']';
        }
        head[n] = '\0';
        window(head, 0, _cols, row1);
    }

    // ---- 10. flow -----------------------------------------------------------
    // Classic's header, with rows 2-3 as one block. A long title wraps over both
    // rows instead of scrolling (each scroll step is a 14-frame repaint, ~700 ms);
    // the block then pages between the title and artist/album on the wall clock.
    // A title that fits: no pages, artist (plus album if it fits) below it.
    void MediaScreen::flow(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        header(row1);

        const uint8_t tl = (uint8_t)strlen(_title);

        // An album named like the title (a single) is dropped.
        const char* album = _album;
        {
            const char* p = _album; const char* q = _title;
            while (*p && *q && ((*p | 0x20) == (*q | 0x20))) { ++p; ++q; }
            if (!*p && !*q) album = "";
        }

        if (tl <= _cols) {
            char info[ROW_MAX * 4];
            if (_artist[0] && album[0] && strlen(_artist) + 3 + strlen(album) <= _cols)
                snprintf(info, sizeof(info), "%s - %s", _artist, album);
            else
                snprintf(info, sizeof(info), "%s", _artist[0] ? _artist : album);
            window(_title, 0, _cols, row2);
            window(info, offsetFor((uint8_t)strlen(info), nowMs, _scrollMsArtist), _cols, row3);
            return;
        }

        // No artist or album: no paging.
        const bool     haveInfo = _artist[0] || album[0];
        const uint32_t cycle    = FLOW_TITLE_MS + FLOW_INFO_MS;
        if (haveInfo && (nowMs % cycle) >= FLOW_TITLE_MS) {
            const char* top = _artist[0] ? _artist : album;
            const char* bot = (_artist[0] && album[0]) ? album : "";
            window(top, offsetFor((uint8_t)strlen(top), nowMs, _scrollMsArtist), _cols, row2);
            window(bot, offsetFor((uint8_t)strlen(bot), nowMs, _scrollMsArtist), _cols, row3);
            return;
        }

        if (wrapTwo(_title, _cols, row2, row3)) return;

        // Longer than two rows: hold the opening, scroll the remainder.
        memcpy(row2, _title, _cols);
        row2[_cols] = '\0';
        window(_title + _cols,
               offsetFor((uint8_t)strlen(_title + _cols), nowMs, _scrollMsTitle),
               _cols, row3);
    }

    void MediaScreen::composeSplash(char* row1, char* row2, char* row3, uint8_t cols) const
    {
        if (cols > ROW_MAX - 1) cols = ROW_MAX - 1;

        // As in Flow: an album named like the title is dropped.
        const char* album = _album;
        {
            const char* p = _album; const char* q = _title;
            while (*p && *q && ((*p | 0x20) == (*q | 0x20))) { ++p; ++q; }
            if (!*p && !*q) album = "";
        }

        char a[ROW_MAX], b[ROW_MAX], c[ROW_MAX];
        if (strlen(_title) <= cols) {
            window(_title, 0, cols, a);
            window(_artist, 0, cols, b);
            window(album, 0, cols, c);
        } else {
            if (!wrapTwo(_title, cols, a, b)) {       // past two rows: the opening is what names it
                window(_title, 0, cols, a);
                window(_title + cols, 0, cols, b);
            }
            window(_artist[0] ? _artist : album, 0, cols, c);
        }
        centre(a, cols, row1);
        centre(b, cols, row2);
        centre(c, cols, row3);
    }

    bool MediaScreen::wrapTwo(const char* s, uint8_t cols, char* a, char* b)
    {
        const size_t len = strlen(s);
        if (len > (size_t)cols * 2) return false;
        if (len <= cols) { window(s, 0, cols, a); b[0] = '\0'; return true; }

        // Break at the last space before the edge, but only if the rest fits
        // row two; otherwise cut at the edge. Unlike BigTitle, never truncate.
        size_t brk = cols, skip = 0;
        for (size_t i = cols; i > 0; --i) {
            if (s[i] != ' ') continue;
            if (len - i - 1 <= cols) { brk = i; skip = 1; }
            break;
        }
        memcpy(a, s, brk);
        a[brk] = '\0';
        window(s + brk + skip, 0, cols, b);
        return true;
    }

    // ---- 1. minimal ---------------------------------------------------------
    // Clock, title, artist. The cheapest screen on the bus.
    void MediaScreen::minimal(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        char head[ROW_MAX];
        snprintf(head, sizeof(head), "%s", _clock[0] ? _clock : "");
        window(head, 0, _cols, row1);
        titleRow(nowMs, row2);
        artistRow(nowMs, row3);
    }

    // ---- 2. bar -------------------------------------------------------------
    // Title, artist, and the bottom row as a full-width progress bar.
    void MediaScreen::bar(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        titleRow(nowMs, row1);
        artistRow(nowMs, row2);

        char pos[16];
        position(pos, sizeof(pos));
        char line[ROW_MAX];

        if (!_hasTransport) {
            // No position: the clock only.
            snprintf(line, sizeof(line), "%s", _clock[0] ? _clock : "");
            window(line, 0, _cols, row3);
            return;
        }

        // Row minus position and a space; dropped below three cells.
        const int posLen = (int)strlen(pos);
        int inner = (int)_cols - posLen - 1 - 2;      // -1 space, -2 brackets
        if (inner > (int)ROW_MAX - 4) inner = (int)ROW_MAX - 4;
        if (inner >= 3) {
            char b[ROW_MAX];
            progressBar(b, sizeof(b), (uint8_t)inner);
            snprintf(line, sizeof(line), "%s %s", b, pos);
        } else {
            snprintf(line, sizeof(line), "%s", pos);
        }
        window(line, 0, _cols, row3);
    }

    // ---- 3. bigtitle --------------------------------------------------------
    // Title over two rows, artist below. Wraps on a space in the second half of
    // row one if there is one. Longer than two rows: row one holds the opening,
    // row two scrolls.
    void MediaScreen::bigTitle(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        const uint8_t len = (uint8_t)strlen(_title);

        if (len <= _cols) {
            window(_title, 0, _cols, row1);
            row2[0] = '\0';
        } else if (len <= (uint16_t)_cols * 2) {
            uint8_t brk = _cols;
            for (uint8_t i = _cols; i > _cols / 2; --i) {
                if (_title[i] == ' ') { brk = i; break; }
            }
            memcpy(row1, _title, brk);
            row1[brk] = '\0';
            const char* rest = _title + brk;
            while (*rest == ' ') ++rest;
            window(rest, 0, _cols, row2);
        } else {
            memcpy(row1, _title, _cols);
            row1[_cols] = '\0';
            window(_title + _cols,
                   offsetFor((uint8_t)strlen(_title + _cols), nowMs, _scrollMsTitle),
                   _cols, row2);
        }
        artistRow(nowMs, row3);
    }

    // ---- 4. ticker ----------------------------------------------------------
    // Three rows as consecutive windows onto one scrolling band. The most
    // expensive style on the bus.
    void MediaScreen::ticker(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        char band[ROW_MAX * 4];
        char pos[16];
        position(pos, sizeof(pos));

        if (pos[0])
            snprintf(band, sizeof(band), "%s   -   %s   -   %s %s",
                     _title[0] ? _title : "---", _artist, glyph(), pos);
        else
            snprintf(band, sizeof(band), "%s   -   %s",
                     _title[0] ? _title : "---", _artist);

        // Windows `_cols` apart on one offset, stepping at the title rate.
        // 7 == kGapLen.
        const uint8_t off = offsetFor((uint8_t)strlen(band), nowMs, _scrollMsTitle);
        const uint16_t period = (uint16_t)(strlen(band) + 7);
        window(band, off, _cols, row1);
        window(band, (uint8_t)((off + _cols) % period), _cols, row2);
        window(band, (uint8_t)((off + _cols * 2) % period), _cols, row3);
    }

    // ---- 5. split -----------------------------------------------------------
    // Artist above title; position on the header, no bar.
    void MediaScreen::split(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        char pos[16];
        position(pos, sizeof(pos));
        char head[ROW_MAX];
        if (_clock[0] && pos[0]) snprintf(head, sizeof(head), "%s  %s %s", _clock, glyph(), pos);
        else if (_clock[0])      snprintf(head, sizeof(head), "%s", _clock);
        else if (pos[0])         snprintf(head, sizeof(head), "%s %s", glyph(), pos);
        else                     head[0] = '\0';
        window(head, 0, _cols, row1);
        artistRow(nowMs, row2);
        titleRow(nowMs, row3);
    }

    // ---- 6. pulse (animated) ------------------------------------------------
    // A sweep on row 3: right while playing, left while rewinding, still when
    // paused.
    void MediaScreen::pulse(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        titleRow(nowMs, row1);
        artistRow(nowMs, row2);

        char line[ROW_MAX];
        const uint8_t w = _cols;
        memset(line, '-', w);
        line[w] = '\0';

        if (_hasTransport && _state != State::Paused) {
            // 100 ms per cell, from the wall clock so skipped repaints cost nothing.
            const uint8_t phase = (uint8_t)((nowMs / 100) % w);
            const bool back = (_state == State::Rewinding);
            for (uint8_t i = 0; i < 3 && i < w; ++i) {
                const uint8_t p = back ? (uint8_t)((w + phase - i) % w)
                                       : (uint8_t)((phase + i) % w);
                line[p] = (i == 0) ? '=' : '#';
            }
        }

        // Position at the right end, if it fits.
        char pos[16];
        position(pos, sizeof(pos));
        const uint8_t plen = (uint8_t)strlen(pos);
        if (plen && w > plen + 2) {
            line[w - plen - 1] = ' ';
            memcpy(line + w - plen, pos, plen);
        }
        window(line, 0, _cols, row3);
    }

    // ---- 7. scanner (animated) ----------------------------------------------
    // A cell bouncing along row 1 over title and artist: shows the firmware is
    // alive and the bus is carrying screens.
    void MediaScreen::scanner(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        char line[ROW_MAX];
        const uint8_t w = _cols;
        memset(line, ' ', w);
        line[w] = '\0';

        // Triangle wave 0..w-1 and back, no doubled end frame.
        const uint16_t span = (uint16_t)(w > 1 ? (w - 1) * 2 : 1);
        const uint16_t t = (uint16_t)((nowMs / 120) % span);
        const uint8_t p = (uint8_t)(t < (uint16_t)(w - 1) ? t : span - t);
        line[p] = '*';
        if (p > 0) line[p - 1] = '-';
        if (p + 1 < w) line[p + 1] = '-';

        // Clock written last, at the right end, so the sweep never hides it.
        if (_clock[0] && w > 8) memcpy(line + w - 5, _clock, 5);

        window(line, 0, _cols, row1);
        titleRow(nowMs, row2);
        artistRow(nowMs, row3);
    }

    // ---- 8. clock -----------------------------------------------------------
    // Letter-spaced, centred clock ("1 2 : 3 4") over title and artist; the
    // panel has one font size. Falls back to the track position with no clock.
    void MediaScreen::clockBig(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        char wide[ROW_MAX];
        uint8_t n = 0;
        if (_clock[0]) {
            for (const char* p = _clock; *p && n < ROW_MAX - 2; ++p) {
                wide[n++] = *p;
                if (*(p + 1) && n < ROW_MAX - 2) wide[n++] = ' ';
            }
        }
        wide[n] = '\0';
        if (!n) position(wide, sizeof(wide));

        centre(wide, _cols, row1);
        titleRow(nowMs, row2);
        artistRow(nowMs, row3);
    }

    // ---- 9. vu (animated) ---------------------------------------------------
    // A fake level meter: there is no audio here (AMS carries metadata only).
    // A deterministic wobble seeded from time and title; still when paused.
    // ---- plain --------------------------------------------------------------
    // Title, artist, album, each on its own row; the album row uses the
    // artist's scroll rate.
    void MediaScreen::plain(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        window(_title[0] ? _title : "---", offsetFor((uint8_t)strlen(_title), nowMs, _scrollMsTitle),
               _cols, row1);
        artistRow(nowMs, row2);
        window(_album, offsetFor((uint8_t)strlen(_album), nowMs, _scrollMsArtist), _cols, row3);
    }

    // ---- winamp -------------------------------------------------------------
    // Row 1: the clock, then a spectrum of bars that dance while something is
    // up (the head unit sends no play state, so a title is enough). Row 2: the
    // classic marquee, "Artist - Title *** " looping. Row 3: the album.
    void MediaScreen::winamp(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        const uint8_t w = _cols;
        char line[ROW_MAX];
        uint8_t n = 0;
        if (_clock[0]) n = (uint8_t)snprintf(line, sizeof(line), "%s ", _clock);
        if (n >= w) n = 0;

        // Levels low to high; a hash of the instant, so dirty() is stable.
        static const char kBars[] = " .:|";
        const bool moving = _title[0] && (!_hasTransport || _state != State::Paused);
        const uint32_t tick = moving ? nowMs / 120 : 0;
        for (uint8_t i = 0; n < w && n < ROW_MAX - 1; ++i) {
            uint32_t h = (tick * 131u + i * 2654435761u) ^ (tick >> 3);
            h ^= h >> 15;
            uint8_t lvl = moving ? (uint8_t)(h % 4) : 1;
            if (moving && i < 4 && lvl < 3) ++lvl;   // lower bands sit higher, as music does
            line[n++] = kBars[lvl];
        }
        line[n] = '\0';
        window(line, 0, w, row1);

        char band[ROW_MAX * 3];
        if (_artist[0]) snprintf(band, sizeof(band), "%s - %s *** ", _artist, _title[0] ? _title : "---");
        else            snprintf(band, sizeof(band), "%s *** ", _title[0] ? _title : "---");
        // Always moving, the way Winamp's was, unless it fits.
        const uint8_t len = (uint8_t)strlen(band);
        const uint16_t step = _scrollMsTitle ? _scrollMsTitle : 300;
        uint8_t off = 0;
        if (len > w) off = (uint8_t)((nowMs / step) % len);
        if (len > w) {
            for (uint8_t i = 0; i < w; ++i) row2[i] = band[(off + i) % len];
            row2[w] = '\0';
        } else {
            window(band, 0, w, row2);
        }

        window(_album, offsetFor((uint8_t)strlen(_album), nowMs, _scrollMsArtist), w, row3);
    }

    void MediaScreen::vu(uint32_t nowMs, char* row1, char* row2, char* row3) const
    {
        titleRow(nowMs, row1);
        artistRow(nowMs, row2);

        char line[ROW_MAX];
        const uint8_t w = _cols;
        const bool moving = _hasTransport && _state != State::Paused;

        // Two bars so it is not mistaken for a progress bar.
        uint8_t seed = 0;
        for (const char* p = _title; *p; ++p) seed = (uint8_t)(seed * 31 + (uint8_t)*p);

        const uint8_t half = (uint8_t)(w > 3 ? (w - 1) / 2 : 1);
        const uint32_t tick = moving ? nowMs / 150 : 0;
        auto level = [&](uint8_t chan) -> uint8_t {
            // A hash, not random: dirty() needs the same instant to give the same screen.
            uint32_t h = (tick + chan * 7u + seed) * 2654435761u;
            h ^= h >> 13;
            return (uint8_t)(moving ? (h % (half + 1)) : (half / 3));
        };

        const uint8_t l = level(0), rr = level(1);
        uint8_t n = 0;
        for (uint8_t i = 0; i < half && n < ROW_MAX - 2; ++i)
            line[n++] = (i < l) ? '#' : ' ';
        if (n < ROW_MAX - 2) line[n++] = '|';
        for (uint8_t i = 0; i < half && n < ROW_MAX - 2; ++i)
            line[n++] = (i < rr) ? '#' : ' ';
        line[n] = '\0';
        window(line, 0, _cols, row3);
    }

    bool MediaScreen::dirty(uint32_t nowMs) const
    {
        char a[ROW_MAX], b[ROW_MAX], c[ROW_MAX];
        compose(nowMs, a, b, c);
        return strcmp(a, _lastRow1) != 0 ||
               strcmp(b, _lastRow2) != 0 ||
               strcmp(c, _lastRow3) != 0;
    }

    void MediaScreen::markRendered(uint32_t nowMs)
    {
        compose(nowMs, _lastRow1, _lastRow2, _lastRow3);
    }
}
