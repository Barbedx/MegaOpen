#include "MediaPane.h"
#include "PaneImages.h"
#include <string.h>
#include "../utils/UiList.h"

namespace MegaOpen
{
    // ---- the font --------------------------------------------------------
    const MediaPane::Glyph MediaPane::kDigits[10] = {
        {{0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70}}, // 0
        {{0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70}}, // 1
        {{0x70, 0x88, 0x08, 0x10, 0x20, 0x40, 0xF8}}, // 2
        {{0x70, 0x88, 0x08, 0x30, 0x08, 0x88, 0x70}}, // 3
        {{0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10}}, // 4
        {{0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70}}, // 5
        {{0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70}}, // 6
        {{0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40}}, // 7
        {{0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70}}, // 8
        {{0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60}}, // 9
    };
    const MediaPane::Glyph MediaPane::kColon = {{0x00, 0x20, 0x20, 0x00, 0x20, 0x20, 0x00}};

    // ---- names -----------------------------------------------------------
    // One table, walked both ways and served by /api/catalog; the static_assert
    // keeps it in step with the enum.
    static const UiName kScenes[] = {
        {"off",    nullptr},
        {"media",  nullptr},
        {"vu",     "VU"},
        {"wave",   nullptr},
        {"clock",  nullptr},
        {"stars",  nullptr},
        {"bounce", nullptr},
        {"rings",  nullptr},
        {"spiral", nullptr},
        {"scope",  nullptr},
        {"rain",   nullptr},
        {"radar",  nullptr},
        {"life",   nullptr},
        {"tape",   nullptr},
        {"tunnel", nullptr},
        {"gmeter", nullptr},
        {"tryzub", nullptr},
        {"tryzubclock", nullptr},
        {"renault", nullptr},
        {"custom", nullptr},
    };
    static_assert(sizeof(kScenes) / sizeof(kScenes[0]) == (size_t)MediaPane::SCENE_COUNT,
                  "MediaPane scene name table drifted from the enum");

    const char* MediaPane::sceneName(Scene s)
    {
        const uint8_t i = (uint8_t)s;
        return kScenes[i < SCENE_COUNT ? i : 0].id;
    }

    const char* MediaPane::sceneTitle(Scene s)
    {
        const uint8_t i = (uint8_t)s;
        return kScenes[i < SCENE_COUNT ? i : 0].name;
    }

    MediaPane::Scene MediaPane::sceneFromName(const char* s)
    {
        if (!s) return Scene::Off;
        for (uint8_t i = 0; i < SCENE_COUNT; ++i)
            if (!strcmp(s, kScenes[i].id)) return (Scene)i;
        return Scene::Off;
    }

    bool MediaPane::animates(Scene s)
    {
        // Media counts: its spectrum animates while playing; TryzubClock's
        // minute changes.
        return s != Scene::Off && s != Scene::Custom && s != Scene::Tryzub && s != Scene::Renault;
    }

    bool MediaPane::usesMedia(Scene s) { return s == Scene::Media || s == Scene::Tape; }

    MediaPane::Scene MediaPane::carouselNext(Scene s)
    {
        uint8_t i = (uint8_t)((uint8_t)s + 1);
        // Wrap past Off, never onto it; GMeter, the pictures and Custom are not stops.
        if (i >= (uint8_t)Scene::GMeter) i = 1;
        return (Scene)i;
    }

    void MediaPane::toHex(const uint8_t* bits, char* out)
    {
        static const char kHex[] = "0123456789abcdef";
        for (uint16_t i = 0; i < BYTES; ++i) {
            out[i * 2]     = kHex[bits[i] >> 4];
            out[i * 2 + 1] = kHex[bits[i] & 0x0F];
        }
        out[BYTES * 2] = '\0';
    }

    bool MediaPane::fromHex(const char* hex, uint8_t* out)
    {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        if (!hex) return false;
        for (uint16_t i = 0; i < BYTES * 2; ++i)
            if (nib(hex[i]) < 0) return false;       // a short string stops at its NUL
        if (hex[BYTES * 2] != '\0') return false;
        if (out)
            for (uint16_t i = 0; i < BYTES; ++i)
                out[i] = (uint8_t)(nib(hex[i * 2]) << 4 | nib(hex[i * 2 + 1]));
        return true;
    }

    // ---- primitives ------------------------------------------------------
    void MediaPane::clear(uint8_t* b) { memset(b, 0, BYTES); }

    void MediaPane::px(uint8_t* b, int x, int y)
    {
        // Bounds-checked here: coordinates derive from phone-supplied position
        // and duration (AMS can report elapsed > duration across a seek).
        if (x < 0 || x >= (int)W || y < 0 || y >= (int)H) return;
        b[y * STRIDE + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
    }

    // Clip the loop range too, not only in px(): a huge span would otherwise
    // iterate millions of off-pane pixels on the loop task.
    void MediaPane::hline(uint8_t* b, int x0, int x1, int y)
    {
        if (y < 0 || y >= (int)H) return;
        if (x0 < 0) x0 = 0;
        if (x1 > (int)W - 1) x1 = (int)W - 1;
        for (int x = x0; x <= x1; ++x) px(b, x, y);
    }

    void MediaPane::vline(uint8_t* b, int x, int y0, int y1)
    {
        if (x < 0 || x >= (int)W) return;
        if (y0 < 0) y0 = 0;
        if (y1 > (int)H - 1) y1 = (int)H - 1;
        for (int y = y0; y <= y1; ++y) px(b, x, y);
    }

    void MediaPane::rect(uint8_t* b, int x0, int y0, int x1, int y1, bool fill)
    {
        // Same loop bound as hline/vline, on rows.
        if (fill) {
            if (y0 < 0) y0 = 0;
            if (y1 > (int)H - 1) y1 = (int)H - 1;
            for (int y = y0; y <= y1; ++y) hline(b, x0, x1, y);
            return;
        }
        hline(b, x0, x1, y0); hline(b, x0, x1, y1);
        vline(b, x0, y0, y1); vline(b, x1, y0, y1);
    }

    int MediaPane::isin(int deg)
    {
        static const int16_t q[91] = {
            0,18,36,54,71,89,107,125,143,160,178,195,213,230,248,265,282,299,316,333,350,
            367,384,400,416,433,449,465,481,496,512,527,543,558,573,587,602,616,630,644,
            658,672,685,698,711,724,737,749,761,773,784,796,807,818,828,839,849,859,868,
            878,887,896,904,912,920,928,935,942,949,955,962,967,973,978,983,987,992,995,
            999,1002,1005,1008,1010,1012,1013,1014,1015,1016,1016,1016,1016};
        deg = ((deg % 360) + 360) % 360;
        if (deg <= 90)  return q[deg];
        if (deg <= 180) return q[180 - deg];
        if (deg <= 270) return -q[deg - 180];
        return -q[360 - deg];
    }

    void MediaPane::line(uint8_t* b, int x0, int y0, int x1, int y1)
    {
        const int dx = x1 - x0, dy = y1 - y0;
        const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        const int n = ax > ay ? ax : ay;
        if (n == 0) { px(b, x0, y0); return; }
        for (int i = 0; i <= n; ++i) px(b, x0 + dx * i / n, y0 + dy * i / n);
    }

    void MediaPane::circle(uint8_t* b, int cx, int cy, int r)
    {
        // Step ~= 57/r degrees keeps the chord <= 1 px so the ring closes
        // (a fixed 4 degrees gives 1.54 px gaps at r=22).
        int step = r > 0 ? 57 / r : 4;
        if (step < 1) step = 1;
        for (int a = 0; a < 360; a += step)
            px(b, cx + r * icos(a) / 1024, cy + r * isin(a) / 1024);
    }

    void MediaPane::glyph(uint8_t* b, const Glyph& g, int x, int y)
    {
        for (int r = 0; r < 7; ++r)
            for (int c = 0; c < 5; ++c)
                if (g.r[r] & (0x80 >> c)) px(b, x + c, y + r);
    }

    uint8_t MediaPane::timeWidth(uint32_t secs)
    {
        // Must agree with timeAt()'s advances exactly: digit 6, colon 4, and the
        // final digit contributes its 5 columns rather than its advance. 21 for
        // "M:SS", 27 for "MM:SS".
        if (secs > 99 * 60 + 59) secs = 99 * 60 + 59;
        return (secs / 60 >= 10) ? 27 : 21;
    }

    void MediaPane::timeAt(uint8_t* b, int x, int y, uint32_t secs)
    {
        // Saturate rather than wrap: wrapping draws plausible wrong numbers.
        if (secs > 99 * 60 + 59) secs = 99 * 60 + 59;
        const uint32_t m = secs / 60, s = secs % 60;
        int at = x;
        if (m >= 10) { glyph(b, kDigits[m / 10], at, y); at += 6; }
        glyph(b, kDigits[m % 10], at, y); at += 6;
        glyph(b, kColon, at, y);          at += 4;
        glyph(b, kDigits[s / 10], at, y); at += 6;
        glyph(b, kDigits[s % 10], at, y);
    }

    // ---- animation -------------------------------------------------------
    uint32_t MediaPane::next()
    {
        _rng ^= _rng << 13; _rng ^= _rng >> 17; _rng ^= _rng << 5;
        return _rng;
    }

    void MediaPane::seedStar(uint8_t i, bool anywhere)
    {
        _sx[i] = (int16_t)((int32_t)(next() % 2048) - 1024);
        _sy[i] = (int16_t)((int32_t)(next() % 2048) - 1024);
        // Respawns go to the far plane so stars do not pop in mid-tunnel.
        _sz[i] = anywhere ? (int16_t)(1 + next() % 1024) : 1024;
    }

    void MediaPane::seedDrop(uint8_t i, bool anywhere)
    {
        // Fresh drops start a random margin above the pane; `anywhere` (first
        // seeding) spreads heads down the pane.
        _ry[i] = anywhere ? (int8_t)(next() % H) : (int8_t)-(int)(next() % 24);
        _rl[i] = (uint8_t)(5 + next() % 14);
        _rv[i] = (uint8_t)(1 + next() % 3);
    }

    bool MediaPane::lit(const uint8_t* b, int x, int y)
    {
        // Toroidal, so gliders survive the edges.
        x = ((x % (int)W) + (int)W) % (int)W;
        y = ((y % (int)H) + (int)H) % (int)H;
        return (b[y * STRIDE + (x >> 3)] & (0x80 >> (x & 7))) != 0;
    }

    void MediaPane::seedLife()
    {
        // Two random bytes ANDed: quarter density.
        for (uint16_t i = 0; i < BYTES; ++i)
            _life[i] = (uint8_t)(next() & next());
        _lifeAge = 0;
    }

    void MediaPane::ensureSeeded()
    {
        if (_seeded) return;
        _seeded = true;
        for (uint8_t i = 0; i < W; ++i)     _wave[i] = 24;   // flat, mid-height
        for (uint8_t i = 0; i < kStars; ++i) seedStar(i, true);
        for (uint8_t i = 0; i < 8; ++i)     { _btx[i] = 8; _bty[i] = 8; }
        for (uint8_t i = 0; i < kDrops; ++i) seedDrop(i, true);
        for (uint8_t i = 0; i < kBlips; ++i) {
            _bla[i] = (uint8_t)(next() % 180);          // half-degrees
            _blr[i] = (uint8_t)(4 + next() % 16);       // inside the r=22 dish
        }
        seedLife();
    }

    void MediaPane::step()
    {
        ensureSeeded();
        ++_frame;

        switch (_scene) {
            case Scene::Vu: {
                // Fake level; the needle eases toward it rather than snapping.
                _vuTarget = (int16_t)(20 + (int)(next() % 120));
                _vuAngle  = (int16_t)(_vuAngle + (_vuTarget - _vuAngle) / 2);
                if (_vuAngle > _vuPeak)          _vuPeak = _vuAngle;
                else if (_vuPeak > _vuAngle + 2) _vuPeak = (int16_t)(_vuPeak - 3);
                return;
            }
            case Scene::Wave: {
                // Shift left, then write one new column.
                for (uint8_t i = 0; i + 1 < W; ++i) _wave[i] = _wave[i + 1];
                _wavePhase = (int16_t)((_wavePhase + 23) % 360);
                const int v = 24 + (18 * isin(_wavePhase) / 1024)
                                 + (9 * isin(_wavePhase * 3) / 1024);
                _wave[W - 1] = (uint8_t)(v < 1 ? 1 : (v > (int)H - 2 ? (int)H - 2 : v));
                return;
            }
            case Scene::Stars:
                for (uint8_t i = 0; i < kStars; ++i) {
                    _sz[i] = (int16_t)(_sz[i] - 40);
                    if (_sz[i] < 40) seedStar(i, false);
                }
                return;
            case Scene::Bounce: {
                _bx = (int8_t)(_bx + _bdx); _by = (int8_t)(_by + _bdy);
                if (_bx < 0)  { _bx = 0;  _bdx = (int8_t)-_bdx; }
                if (_bx > 37) { _bx = 37; _bdx = (int8_t)-_bdx; }
                if (_by < 0)  { _by = 0;  _bdy = (int8_t)-_bdy; }
                if (_by > 37) { _by = 37; _bdy = (int8_t)-_bdy; }
                _btx[_bn] = _bx; _bty[_bn] = _by;
                _bn = (uint8_t)((_bn + 1) & 7);
                return;
            }
            case Scene::Rain:
                for (uint8_t i = 0; i < kDrops; ++i) {
                    _ry[i] = (int8_t)(_ry[i] + (_rv[i] ? _rv[i] : 1));
                    // Respawn once the tail, not the head, has left the pane.
                    if ((int)_ry[i] - (int)_rl[i] > (int)H - 1) seedDrop(i, false);
                }
                return;
            case Scene::Life: {
                // One B3/S23 generation into a scratch board (~20k bit tests).
                uint8_t nxt[BYTES];
                memset(nxt, 0, BYTES);
                uint16_t pop = 0;
                for (int y = 0; y < (int)H; ++y)
                    for (int x = 0; x < (int)W; ++x) {
                        int n = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                                if ((dx || dy) && lit(_life, x + dx, y + dy)) ++n;
                        const bool alive = lit(_life, x, y) ? (n == 2 || n == 3) : (n == 3);
                        if (alive) { px(nxt, x, y); ++pop; }
                    }
                memcpy(_life, nxt, BYTES);
                // Reseed when sparse or old: a static board stops being sent,
                // which looks like a fault on the glass.
                if (++_lifeAge >= kLifeMaxAge || pop < kLifeMinPop) seedLife();
                return;
            }
            // Tape turns its reels by a counter of its own, only while playing.
            case Scene::Tape:
                if (_hasTransport ? _playing : true) ++_tapeTurn;
                return;
            // Rings, Spiral, Scope, Radar and Tunnel are pure functions of _frame, Clock
            // of the time, GMeter of setAccel(), and Off has nothing to advance.
            case Scene::Rings:
            case Scene::Spiral:
            case Scene::Scope:
            case Scene::Radar:
            case Scene::Tunnel:
            case Scene::GMeter:
            case Scene::Clock:
            case Scene::Tryzub:
            case Scene::TryzubClock:
            case Scene::Renault:
            case Scene::Custom:
            case Scene::Off:
                return;
            case Scene::Media:
            default:
                break;
        }

        // No transport means no play state: run the bars as if playing.
        const bool live = _hasTransport ? _playing : true;

        for (uint8_t i = 0; i < kBars; ++i) {
            if (live) {
                // Pulled toward the bar below, then kicked, so neighbours correlate.
                const int pull = (i > 0) ? ((int)_h[i - 1] - (int)_h[i]) / 3 : 0;
                const int kick = (int)(next() % 9) - 4;
                int v = (int)_h[i] + pull + kick;
                if (v < 1)       v = 1;
                if (v > kBarMax) v = kBarMax;
                _h[i] = (uint8_t)v;
            } else if (_h[i] > 1) {
                // Paused: settle to the floor so the image stops changing and
                // the caller can skip the identical 44-frame transfer.
                --_h[i];
            }
            // The peak never decays below its own bar; otherwise a paused pane
            // oscillates 1 <-> 0 for ever and is re-sent every frame.
            if (_h[i] > _peak[i])                              _peak[i] = _h[i];
            else if (_peak[i] > _h[i] && (next() & 3) == 0)    --_peak[i];
        }
    }

    // ---- the pane --------------------------------------------------------
    // 48x48 rows:
    //   0..6    elapsed left, remaining right
    //   9..30   eight spectrum bars
    //   33..36  progress bar
    //   39..45  transport glyph
    // With no transport only the spectrum is drawn (setHasTransport()).
    void MediaPane::drawMedia(uint8_t* b) const
    {
        if (_hasTransport) {
            const uint16_t left = (_duration > _elapsed) ? (uint16_t)(_duration - _elapsed) : 0;

            // Remaining is right-aligned and omitted when both fields do not fit
            // (27 + 27 = 54 > 48): missing beats wrong. Threshold is exactly W;
            // "1:01" + "12:34" = 21 + 27 = 48 fits with the font's own gap.
            const uint8_t wLeft    = timeWidth(left);
            const uint8_t wElapsed = timeWidth(_elapsed);
            timeAt(b, 0, 0, _elapsed);
            if ((int)wElapsed + (int)wLeft <= (int)W)
                timeAt(b, (int)W - (int)wLeft, 0, left);
        }

        // Eight bars, 5 px wide with a 1 px gap: 8*6-1 = 47.
        constexpr int kBase = 30;                 // bars grow upward from here
        for (int i = 0; i < (int)kBars; ++i) {
            const int x0 = i * 6, x1 = x0 + 4;
            const int top = kBase - (int)_h[i];
            rect(b, x0, top, x1, kBase, true);
            const int pk = kBase - (int)_peak[i] - 2;
            if (pk >= 9 && pk < top - 1) hline(b, x0, x1, pk);   // peak-hold marker
        }

        if (!_hasTransport) return;

        rect(b, 0, 33, 47, 36, false);
        if (_duration) {
            const uint32_t e = (_elapsed < _duration) ? _elapsed : _duration;
            const uint32_t fill = (46u * e) / _duration;
            if (fill) rect(b, 1, 34, 1 + (int)fill, 35, true);
        }

        if (_playing) {
            for (int r = 0; r < 7; ++r) {          // a triangle, narrowing as it goes
                const int w = 4 - (r > 3 ? r - 3 : 3 - r);
                for (int c = 0; c <= w; ++c) px(b, 1 + c, 39 + r);
            }
        } else {
            rect(b, 1, 39, 2, 45, true);
            rect(b, 5, 39, 6, 45, true);
        }
    }

    // ---- the decorative six (no data; see usesMedia()) --------------------

    void MediaPane::drawVu(uint8_t* b) const
    {
        const int cx = 24, cy = 42, r = 34;
        for (int a = 20; a <= 160; a += 2)              // the arc
            px(b, cx + r * icos(a) / 1024, cy - r * isin(a) / 1024);
        for (int a = 20; a <= 160; a += 20) {           // ticks
            line(b, cx + (r - 5) * icos(a) / 1024, cy - (r - 5) * isin(a) / 1024,
                    cx + r * icos(a) / 1024,       cy - r * isin(a) / 1024);
        }
        // Drawn twice, one pixel apart: a 1 px needle vanishes on real glass.
        const int a = 160 - _vuAngle;
        line(b, cx,     cy, cx + (r - 7) * icos(a) / 1024, cy - (r - 7) * isin(a) / 1024);
        line(b, cx - 1, cy, cx + (r - 7) * icos(a) / 1024, cy - (r - 7) * isin(a) / 1024);
        const int pa = 160 - _vuPeak;                   // the peak pip, lagging
        px(b, cx + (r - 3) * icos(pa) / 1024, cy - (r - 3) * isin(pa) / 1024);
        rect(b, cx - 2, cy - 2, cx + 2, cy + 2, true);  // the hub
        rect(b, 0, 0, 47, 47, false);                   // border
    }

    void MediaPane::drawWave(uint8_t* b) const
    {
        hline(b, 0, 47, 24);                            // the zero line
        for (int x = 0; x < (int)W; ++x) {
            const int y = _wave[x];
            if (y < 24) rect(b, x, y, x, 24, true);
            else        rect(b, x, 24, x, y, true);
        }
    }

    void MediaPane::drawClock(uint8_t* b) const
    {
        const int cx = 23, cy = 23, r = 22;
        circle(b, cx, cy, r);
        for (int k = 0; k < 12; ++k) {                  // hour marks
            const int a = k * 30;
            line(b, cx + (r - 4) * isin(a) / 1024, cy - (r - 4) * icos(a) / 1024,
                    cx + (r - 2) * isin(a) / 1024, cy - (r - 2) * icos(a) / 1024);
        }
        const uint32_t s = _clockSecs % 60;
        const uint32_t m = (_clockSecs / 60) % 60;
        const uint32_t h = (_clockSecs / 3600) % 12;
        // The hour hand advances with the minutes (h*30 + m/2).
        const int ha = (int)(h * 30 + m / 2), ma = (int)(m * 6), sa = (int)(s * 6);
        line(b, cx,     cy, cx + 10 * isin(ha) / 1024, cy - 10 * icos(ha) / 1024);
        line(b, cx + 1, cy, cx + 10 * isin(ha) / 1024, cy - 10 * icos(ha) / 1024);
        line(b, cx,     cy, cx + 16 * isin(ma) / 1024, cy - 16 * icos(ma) / 1024);
        line(b, cx,     cy, cx + 19 * isin(sa) / 1024, cy - 19 * icos(sa) / 1024);
        rect(b, cx - 1, cy - 1, cx + 1, cy + 1, true);
    }

    // The small tryzub (rows 0..30) and the time under it, HH:MM in the
    // pane's 5x7 digits, centred: 27 columns (digit 6, colon 4, last digit 5).
    void MediaPane::drawTryzubClock(uint8_t* b) const
    {
        memcpy(b, PaneImages::kTryzubSmall, sizeof(PaneImages::kTryzubSmall));
        const uint32_t h = _clockSecs / 3600, m = (_clockSecs / 60) % 60;
        int at = (W - 27) / 2;
        const int y = 37;
        glyph(b, kDigits[h / 10], at, y); at += 6;
        glyph(b, kDigits[h % 10], at, y); at += 6;
        glyph(b, kColon, at, y);          at += 4;
        glyph(b, kDigits[m / 10], at, y); at += 6;
        glyph(b, kDigits[m % 10], at, y);
    }

    void MediaPane::drawStars(uint8_t* b) const
    {
        for (uint8_t i = 0; i < kStars; ++i) {
            if (_sz[i] <= 0) continue;                  // never divide by a dead star
            const int sx = 24 + ((int)_sx[i] * 24) / _sz[i];
            const int sy = 24 + ((int)_sy[i] * 24) / _sz[i];
            if (sx < 0 || sx > 47 || sy < 0 || sy > 47) continue;
            px(b, sx, sy);
            // Near stars drawn fatter.
            if (_sz[i] < 400) { px(b, sx + 1, sy); px(b, sx, sy + 1); }
            if (_sz[i] < 180) px(b, sx + 1, sy + 1);
        }
    }

    void MediaPane::drawBounce(uint8_t* b) const
    {
        // Trail of past positions, older ones sparser, so motion reads at 4 fps.
        for (uint8_t i = 0; i < 8; ++i) {
            const uint8_t k = (uint8_t)((_bn + i) & 7);
            if (i < 5) { px(b, _btx[k] + 5, _bty[k] + 5); px(b, _btx[k] + 5, _bty[k] + 6); }
            else rect(b, _btx[k], _bty[k], _btx[k] + 10, _bty[k] + 10, false);
        }
        rect(b, _bx, _by, _bx + 10, _by + 10, true);
    }

    void MediaPane::drawRings(uint8_t* b) const
    {
        for (int k = 0; k < 3; ++k) {
            const int r = (int)((_frame * 3 + (uint32_t)k * 11) % 33);
            if (r > 1) circle(b, 23, 23, r);
        }
        rect(b, 21, 21, 25, 25, false);
    }

    // ---- the Winamp five (no data) -----------------------------------------
    // Pure functions of _frame or small step() state, sized to read at two
    // images a second.

    void MediaPane::drawSpiral(uint8_t* b) const
    {
        // Three arms; r in quarter pixels, 3.5 degrees per step (~1.4 px chord).
        const int rot = (int)((_frame * 10) % 360);
        for (int k = 0; k <= 92; ++k) {
            const int a0 = rot + (k * 7) / 2;
            for (int arm = 0; arm < 3; ++arm) {
                const int a = a0 + arm * 120;
                px(b, 23 + k * icos(a) / 4096, 23 + k * isin(a) / 4096);
            }
        }
        rect(b, 22, 22, 24, 24, true);
    }

    void MediaPane::drawScope(uint8_t* b) const
    {
        // Lissajous x = sin(p*t + phase), y = sin(q*t); ratio changes every 96 frames.
        static const uint8_t kRatio[5][2] = {{1, 2}, {2, 3}, {3, 4}, {3, 5}, {4, 5}};
        const uint8_t* rt = kRatio[(_frame / 96) % 5];
        const int ph = (int)((_frame * 5) % 360);
        for (int t = 0; t < 360; t += 2)
            px(b, 23 + 21 * isin(rt[0] * t + ph) / 1024,
                  23 + 21 * isin(rt[1] * t) / 1024);
        // Corner ticks.
        hline(b, 0, 3, 0);  vline(b, 0, 0, 3);
        hline(b, 44, 47, 0); vline(b, 47, 0, 3);
        hline(b, 0, 3, 47);  vline(b, 0, 44, 47);
        hline(b, 44, 47, 47); vline(b, 47, 44, 47);
    }

    void MediaPane::drawRain(uint8_t* b) const
    {
        // Twelve columns 4 px apart; 2 px head, tail thins in its far half.
        for (uint8_t i = 0; i < kDrops; ++i) {
            const int x = i * 4 + 1;
            const int len = _rl[i] ? _rl[i] : 1;
            for (int k = 0; k < len; ++k) {
                const int y = (int)_ry[i] - k;
                if (k == 0)                        { px(b, x, y); px(b, x + 1, y); }
                else if (k < len / 2 || !(k & 1))  px(b, x, y);
            }
        }
    }

    void MediaPane::drawRadar(uint8_t* b) const
    {
        const int cx = 23, cy = 23, r = 22;
        circle(b, cx, cy, r);
        for (int k = 0; k < 4; ++k) {                    // N E S W ticks
            const int a = k * 90;
            line(b, cx + (r - 3) * isin(a) / 1024, cy - (r - 3) * icos(a) / 1024,
                    cx + (r - 1) * isin(a) / 1024, cy - (r - 1) * icos(a) / 1024);
        }
        // Angles clockwise from the top, like the clock: x from sin, y from -cos.
        const int a = (int)((_frame * 9) % 360);
        line(b, cx, cy, cx + r * isin(a) / 1024, cy - r * icos(a) / 1024);
        // Two dotted ghost sweeps behind as afterglow.
        for (int i = 0; i < r; i += 2)
            px(b, cx + i * isin(a - 12) / 1024, cy - i * icos(a - 12) / 1024);
        for (int i = 0; i < r; i += 3)
            px(b, cx + i * isin(a - 24) / 1024, cy - i * icos(a - 24) / 1024);
        // Blips lit for a third of a turn after the sweep, fat for the first sixth.
        for (uint8_t i = 0; i < kBlips; ++i) {
            const int ba = (int)_bla[i] * 2;
            const int d  = ((a - ba) % 360 + 360) % 360;
            if (d >= 120) continue;
            const int bx = cx + (int)_blr[i] * isin(ba) / 1024;
            const int by = cy - (int)_blr[i] * icos(ba) / 1024;
            px(b, bx, by);
            if (d < 60) { px(b, bx + 1, by); px(b, bx, by + 1); px(b, bx + 1, by + 1); }
        }
        px(b, cx, cy);
    }

    void MediaPane::drawLife(uint8_t* b) const
    {
        memcpy(b, _life, BYTES);
    }

    // A cassette seen face on: shell, label edge, two reels and the tape
    // running down across the head. Each reel's pack of tape is its share of
    // the track (left = to come, right = played), so the right one grows as
    // the song goes. Spokes turn only while playing; with no position (the
    // head unit) the packs split evenly.
    void MediaPane::drawTape(uint8_t* b) const
    {
        rect(b, 0, 4, 47, 44, false);                    // the shell
        hline(b, 2, 45, 6);                              // label edge
        // The head opening, a trapezoid at the bottom.
        line(b, 12, 44, 15, 38); line(b, 35, 44, 32, 38); hline(b, 15, 32, 38);
        px(b, 3, 41); px(b, 44, 41);                     // screw holes

        // Packs 0..7 px over a 5 px reel: at most 12 + 5, the 17 px between
        // the hubs, so they touch and never cross.
        constexpr int PACK = 7;
        int right = PACK / 2, left = PACK - PACK / 2;
        if (_hasTransport && _duration > 0) {
            right = _elapsed >= _duration ? PACK : (int)((uint32_t)_elapsed * PACK / _duration);
            left  = PACK - right;
        }
        const int cy = 20;
        const int cxs[2] = {15, 32}, packs[2] = {left, right};
        const int a0 = (int)(_tapeTurn * 20 % 360);
        for (int r = 0; r < 2; ++r) {
            const int cx = cxs[r];
            circle(b, cx, cy, 4);                        // the hub
            circle(b, cx, cy, 5 + packs[r]);             // the pack's edge
            for (int k = 0; k < 3; ++k) {                // three spokes
                const int a = a0 + k * 120;
                line(b, cx + isin(a) / 1024, cy - icos(a) / 1024,
                        cx + 4 * isin(a) / 1024, cy - 4 * icos(a) / 1024);
            }
        }
        // The tape leaves each pack at its foot and runs across the head.
        line(b, cxs[0], cy + 5 + left, 13, 36);
        line(b, cxs[1], cy + 5 + right, 34, 36);
        hline(b, 13, 34, 36);
    }

    // Squares rushing outward: each grows by a fixed ratio per step (it doubles
    // every 24 phase units, so equal steps look like equal distances), and is
    // reborn small at the back once it has passed the border. The far ones
    // sit on a vanishing point that drifts on a slow Lissajous, the near ones
    // on the pane's centre. A pure function of _frame.
    void MediaPane::drawTunnel(uint8_t* b) const
    {
        // 256 * 2^(i/24).
        static const uint16_t kExp[24] = {256, 264, 271, 279, 287, 296, 304, 313, 323, 332, 342, 352,
                                          362, 373, 384, 395, 406, 418, 431, 443, 456, 470, 483, 497};
        const int vx = 23 + 7 * isin((int)(_frame * 3)) / 1024;
        const int vy = 23 + 7 * isin((int)(_frame * 5) + 90) / 1024;
        constexpr int SQUARES = 8, SPAN = 12, CYCLE = SQUARES * SPAN;   // phase 0..95
        for (int i = 0; i < SQUARES; ++i) {
            const int ph = (int)((i * SPAN + _frame * 2) % CYCLE);
            const int h = (3 << (ph / 24)) * kExp[ph % 24] / 256;      // half-size 3..47
            const int near = h > 24 ? 24 : h;
            const int x = vx + (23 - vx) * near / 24, y = vy + (23 - vy) * near / 24;
            rect(b, x - h, y - h, x + h, y + h, false);
        }
    }

    void MediaPane::setAccel(int16_t latMg, int16_t longMg, bool fresh, bool verified, uint32_t nowMs)
    {
        if (!fresh) {
            _gLive = false;
            _gn = 0;
            _gHavePeak = false;
            return;
        }
        _gVerified = verified;
        auto clampMg = [](int32_t v) {
            return v > G_FULL_MG ? (int32_t)G_FULL_MG : v < -G_FULL_MG ? -(int32_t)G_FULL_MG : v;
        };
        const int32_t lat = clampMg(latMg), lon = clampMg(longMg);
        for (uint8_t i = kGTrail - 1; i > 0; --i) { _gx[i] = _gx[i - 1]; _gy[i] = _gy[i - 1]; }
        _gx[0] = (int8_t)(G_LAT_SIGN * lat * G_R / G_FULL_MG);
        _gy[0] = (int8_t)(G_LONG_SIGN * lon * G_R / G_FULL_MG);
        if (_gn < kGTrail) ++_gn;
        ++_gSeq;
        _gLive = true;
        // The peak: the strongest pull in the last PEAK_MS; a new one at least
        // as strong takes its place and restarts the hold.
        const int32_t mag = lat * lat + lon * lon;
        if (!_gHavePeak || mag >= _gpMag || (uint32_t)(nowMs - _gpMs) >= PEAK_MS) {
            _gpMag = mag; _gpMs = nowMs;
            _gpx = _gx[0]; _gpy = _gy[0];
            _gHavePeak = true;
        }
    }

    // A ring at 0.5 g (dotted while the decode is unverified), ticks on the
    // axes at 1 g, the dot (3x3), its trail (the two newest points solid,
    // older ones every other image: dimmer), and the peak as an x. Not fresh:
    // a dotted ring and the centre, still.
    void MediaPane::drawGMeter(uint8_t* b) const
    {
        constexpr int C = 23, HALF = G_R / 2;
        auto dotted = [b] {
            constexpr int step = 57 / HALF;
            for (int a = 0, n = 0; a < 360; a += step, ++n)
                if (!(n & 1)) px(b, C + HALF * icos(a) / 1024, C + HALF * isin(a) / 1024);
        };
        if (!_gLive) {
            dotted();
            px(b, C, C);
            return;
        }
        if (_gVerified) circle(b, C, C, HALF);
        else dotted();
        hline(b, C - G_R, C - G_R + 2, C); hline(b, C + G_R - 2, C + G_R, C);
        vline(b, C, C - G_R, C - G_R + 2); vline(b, C, C + G_R - 2, C + G_R);
        px(b, C, C);
        for (uint8_t i = _gn; i-- > 1;)
            if (i <= 2 || ((_gSeq + i) & 1)) px(b, C + _gx[i], C + _gy[i]);
        if (_gHavePeak && (_gpx != _gx[0] || _gpy != _gy[0])) {
            const int x = C + _gpx, y = C + _gpy;
            px(b, x - 1, y - 1); px(b, x + 1, y - 1); px(b, x, y);
            px(b, x - 1, y + 1); px(b, x + 1, y + 1);
        }
        rect(b, C + _gx[0] - 1, C + _gy[0] - 1, C + _gx[0] + 1, C + _gy[0] + 1, true);
    }

    void MediaPane::compose(uint8_t* out) const
    {
        clear(out);
        switch (_scene) {
            case Scene::Media:  drawMedia(out);  break;
            case Scene::Vu:     drawVu(out);     break;
            case Scene::Wave:   drawWave(out);   break;
            case Scene::Clock:  drawClock(out);  break;
            case Scene::Stars:  drawStars(out);  break;
            case Scene::Bounce: drawBounce(out); break;
            case Scene::Rings:  drawRings(out);  break;
            case Scene::Spiral: drawSpiral(out); break;
            case Scene::Scope:  drawScope(out);  break;
            case Scene::Rain:   drawRain(out);   break;
            case Scene::Radar:  drawRadar(out);  break;
            case Scene::Life:   drawLife(out);   break;
            case Scene::Tape:   drawTape(out);   break;
            case Scene::Tunnel: drawTunnel(out); break;
            case Scene::GMeter: drawGMeter(out); break;
            case Scene::Tryzub:  memcpy(out, PaneImages::kTryzub, BYTES);  break;
            case Scene::TryzubClock: drawTryzubClock(out); break;
            case Scene::Renault: memcpy(out, PaneImages::kRenault, BYTES); break;
            case Scene::Custom: if (_custom) memcpy(out, _custom, BYTES); break;
            // Off composes an empty pane: no capture shows a "remove layer"
            // command, so an all-zero image is the only way to blank the glass.
            case Scene::Off:
            default:            break;
        }
    }
}
