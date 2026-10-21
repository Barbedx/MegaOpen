#pragma once
#include <stdint.h>
#include <stddef.h>

// The 48x48 graphic half of the now-playing screen, drawn on the device.
//
// AffaDisplay's showNavBitmap() sends it as the OEM 0x1F1 transfer: 288 bytes,
// row-major, 6 bytes per row, MSB-first, bit 7 of byte 0 the top-left pixel.
//
// What shares the glass with it (measured on the panel):
//   setText (the single line)     -> shares. Text on its half, picture on ours.
//   showInfoMenu (the 3 rows)     -> shares. Same split.
//   showPopupText                 -> draws over the top of both.
//   showMenu / showFullscreenText -> full screen, covers the pane.
// The layer persists under a full-screen text and reappears after it, but is not
// visible meanwhile. Use a compatible layout: MediaScreen::Layout, sharesWithPane().
//
// Pure (no Arduino include, no allocation, no clock) so it runs on the host tests.
//
// The library borrows the bytes, it does not copy them: showNavBitmap() hands
// the pointer to enqueueSplit(), so the buffer must stay valid and unchanged
// until the ticket completes. See the double buffer in main.cpp.
namespace MegaOpen
{
    class MediaPane {
    public:
        static constexpr uint16_t W      = 48;
        static constexpr uint16_t H      = 48;
        static constexpr uint16_t STRIDE = 6;
        static constexpr uint16_t BYTES  = STRIDE * H;   // 288, and the library agrees

        // Procedural scenes, not baked frames. One pane is 44 ACKed CAN frames,
        // so 3-5 fps at best: scenes lean on trails, peak-holds and sweeps.
        // Only Media shows real data.
        enum class Scene : uint8_t {
            Off = 0,   // an empty pane. The only "remove the layer" this panel has.
            Media,     // elapsed/remaining, spectrum, progress, transport — REAL DATA
            Vu,        // an arc, a swinging needle, a lagging peak pip
            Wave,      // a waveform scrolling one column per frame
            Clock,     // an analogue face with a second hand, on the panel's own time
            Stars,     // integer-perspective starfield
            Bounce,    // a box with a decaying trail
            Rings,     // three expanding circles
            // Names are what is stored, so a new carousel scene may go anywhere
            // before GMeter; GMeter and Custom stay last (the carousel wraps at
            // GMeter).
            Spiral,    // three arms turning, pure function of the frame counter
            Scope,     // a Lissajous figure, its ratio changing every 96 frames
            Rain,      // twelve columns of falling dashes, Matrix-style
            Radar,     // a sweep with a fading trail, and blips it lights as it passes
            Life,      // Conway's Life on a torus, reseeded when it dies down
            Tape,      // a cassette: reels turn while playing, tape wound by the position — REAL DATA
            Tunnel,    // nested squares rushing outward from a drifting centre
            GMeter,    // the car's lateral/longitudinal g (0x2A0): a dot, its trail, the peak — REAL DATA
            Custom,    // the owner's own 48x48 picture (POST /api/media bits=), still
            _Count
        };
        static constexpr uint8_t SCENE_COUNT = (uint8_t)Scene::_Count;

        // Carousel: every scene but Off, GMeter and Custom, in enum order,
        // wrapping. Custom stays out: a still picture the owner picks on
        // purpose, not a decoration to rotate through; GMeter likewise (most
        // boards have no car data, and it would show a dotted ring). The ui
        // task owns the timing (Cfg::Settings::paneCycleS).
        static Scene carouselNext(Scene s);

        // Moves on its own? Media stops when paused and settled, Custom never
        // moves; the caller skips identical frames.
        static bool animates(Scene s);
        // Shows real phone/head-unit data? Media, and Tape's position.
        static bool usesMedia(Scene s);

        // Stored by name, never by index.
        static const char* sceneName(Scene s);
        static const char* sceneTitle(Scene s);     // Ukrainian, for /api/catalog
        static Scene sceneFromName(const char* s);

        void setScene(Scene s) { _scene = (uint8_t)s < SCENE_COUNT ? s : Scene::Off; }
        Scene scene() const { return _scene; }
        bool on() const { return _scene != Scene::Off; }

        void setProgress(uint16_t elapsedSec, uint16_t durationSec) {
            _elapsed = elapsedSec; _duration = durationSec;
        }
        void setPlaying(bool p) { _playing = p; }

        // Seconds since midnight for the Clock scene: the header's HH:MM plus a
        // local second counter, not uptime.
        void setClockSecs(uint32_t s) { _clockSecs = s % 86400u; }

        // False when the source has no play state or position (the Raise UART
        // head unit has neither): the pane then drops times, bar and glyph and
        // keeps only the spectrum.
        void setHasTransport(bool on) { _hasTransport = on; }

        // The Custom scene's picture, BYTES in the pane's own format. Borrowed,
        // not copied: the ui task points it at Staged<PaneBits>'s live buffer,
        // which httpd never writes, and re-points it after every apply. A copy
        // of the pane (the web's Mirror) carries the pointer too, and must
        // NOT compose Custom: the buffer is the ui task's. Null draws dark.
        void setCustom(const uint8_t* bits) { _custom = bits; }

        // The G-meter's input, in milli-g: + lateral draws right, + longitudinal
        // (speeding up) draws up. `live` = fresh and verified; when it is not,
        // the scene is a dotted ring and nothing moves, and the trail and the
        // peak are dropped. Each live call is one trail point (the ui task calls
        // once per image, ~2/s); the peak is held PEAK_MS of nowMs.
        void setAccel(int16_t latMg, int16_t longMg, bool live, uint32_t nowMs);
        // accel_long is m/s² x100; the meter works in milli-g.
        static int32_t mgFromMs2x100(int32_t v) { return v * 1000 / 981; }
        static constexpr int16_t  G_FULL_MG = 1000;   // the dot's reach, G_R px
        static constexpr int8_t   G_R       = 22;
        static constexpr uint32_t PEAK_MS   = 3000;

        // BYTES as 2*BYTES lowercase hex characters plus NUL, and back. fromHex
        // takes exactly 2*BYTES hex digits (either case) and nothing more; it
        // writes `out` only when the whole string is valid, and `out` may be
        // null to validate alone.
        static void toHex(const uint8_t* bits, char* out);
        static bool fromHex(const char* hex, uint8_t* out);
        bool hasTransport() const { return _hasTransport; }

        // Advance by one frame. Not wall-clock driven (unlike MediaScreen): the
        // random walks have no "correct position now", so one transfer = one frame.
        void step();

        // Draw into `out` (at least BYTES). Pure: same state, same bytes.
        void compose(uint8_t* out) const;

        // ---- primitives, public so a new scene is a function and not a fork ---
        static void clear(uint8_t* b);
        static void px(uint8_t* b, int x, int y);
        static void hline(uint8_t* b, int x0, int x1, int y);
        static void vline(uint8_t* b, int x, int y0, int y1);
        static void rect(uint8_t* b, int x0, int y0, int x1, int y1, bool fill);
        static void line(uint8_t* b, int x0, int y0, int x1, int y1);
        static void circle(uint8_t* b, int cx, int cy, int r);
        // sin/cos * 1024, integer, one-degree resolution: a 91-entry quadrant
        // table, no libm.
        static int isin(int deg);
        static int icos(int deg) { return isin(deg + 90); }

        // "M:SS" / "MM:SS" left-aligned at x; saturates at 99:59.
        static void timeAt(uint8_t* b, int x, int y, uint32_t secs);
        // Columns timeAt() will occupy, for right-aligning or dropping a field.
        static uint8_t timeWidth(uint32_t secs);

    private:
        // 5x7 font: digits and colon only. One byte per row, top five bits.
        struct Glyph { uint8_t r[7]; };
        static const Glyph kDigits[10];
        static const Glyph kColon;

        static void glyph(uint8_t* b, const Glyph& g, int x, int y);

        // One per Scene.
        void drawMedia(uint8_t* b) const;
        void drawVu(uint8_t* b) const;
        void drawWave(uint8_t* b) const;
        void drawClock(uint8_t* b) const;
        void drawStars(uint8_t* b) const;
        void drawBounce(uint8_t* b) const;
        void drawRings(uint8_t* b) const;
        void drawSpiral(uint8_t* b) const;
        void drawScope(uint8_t* b) const;
        void drawRain(uint8_t* b) const;
        void drawRadar(uint8_t* b) const;
        void drawLife(uint8_t* b) const;
        void drawTape(uint8_t* b) const;
        void drawTunnel(uint8_t* b) const;
        void drawGMeter(uint8_t* b) const;

        // The spectrum is fake: there is no audio (AMS sends metadata, the head
        // unit sends text). A per-bar random walk with decaying peaks.
        // Deterministic (xorshift32, fixed seed) for the host tests.
        static constexpr uint8_t kBars   = 8;
        static constexpr uint8_t kBarMax = 21;
        uint8_t  _h[kBars]    = {4, 9, 14, 11, 7, 12, 6, 3};
        uint8_t  _peak[kBars] = {0};
        uint32_t _rng = 0x1F1A5EED;
        uint32_t next();

        // ---- the decorative scenes' state ------------------------------------
        // All resident (no union) so a scene resumes mid-motion.
        int16_t  _vuAngle = 0, _vuTarget = 0, _vuPeak = 0;

        uint8_t  _wave[W] = {0};        // one height per column, shifted left per frame
        int16_t  _wavePhase = 0;

        // Zero-initialised so an unseeded compose() is safe (the web preview can
        // compose before any step). drawStars() skips _sz == 0 (divide by zero).
        static constexpr uint8_t kStars = 28;
        int16_t  _sx[kStars] = {0}, _sy[kStars] = {0}, _sz[kStars] = {0};
        bool     _seeded = false;
        void seedStar(uint8_t i, bool anywhere);

        int8_t   _bx = 8, _by = 8, _bdx = 3, _bdy = 2;
        uint8_t  _bn = 0;
        int8_t   _btx[8] = {0}, _bty[8] = {0};

        // Rain: head row, tail length and fall speed per column.
        static constexpr uint8_t kDrops = 12;
        int8_t   _ry[kDrops] = {0};
        uint8_t  _rl[kDrops] = {0}, _rv[kDrops] = {0};
        void seedDrop(uint8_t i, bool anywhere);

        // Radar: fixed blips in polar form (half-degrees, radius), lit for a
        // third of a turn after the sweep passes.
        static constexpr uint8_t kBlips = 5;
        uint8_t  _bla[kBlips] = {0}, _blr[kBlips] = {0};

        // Life: cells packed like the pane, so compose() is a memcpy. Reseeded
        // on collapse or after kLifeMaxAge frames (a torus this small settles
        // into blinkers within a minute).
        static constexpr uint16_t kLifeMaxAge = 160;
        static constexpr uint16_t kLifeMinPop = 24;
        uint8_t  _life[BYTES] = {0};
        uint16_t _lifeAge = 0;
        void seedLife();
        static bool lit(const uint8_t* b, int x, int y);

        uint32_t _frame = 0;            // Rings, and anything else counting frames
        uint32_t _tapeTurn = 0;         // Tape's reels: frames spent playing

        const uint8_t* _custom = nullptr;   // Scene::Custom, borrowed (setCustom)

        // GMeter: pixel offsets from the centre, newest first; the peak.
        static constexpr uint8_t kGTrail = 6;
        int8_t   _gx[kGTrail] = {0}, _gy[kGTrail] = {0};
        uint8_t  _gn = 0, _gSeq = 0;
        bool     _gLive = false, _gHavePeak = false;
        int8_t   _gpx = 0, _gpy = 0;
        int32_t  _gpMag = 0;
        uint32_t _gpMs = 0;

        Scene    _scene    = Scene::Off;
        bool     _playing  = false;
        bool     _hasTransport = true;
        uint16_t _elapsed  = 0, _duration = 0;
        uint32_t _clockSecs = 0;

    public:
        // Seeds the decorative scenes (wave at mid-height, stars spread in
        // depth). step() calls it on first use; public so setup() can pay at boot.
        void ensureSeeded();
    };
}
