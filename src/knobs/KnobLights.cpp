#include "KnobLights.h"
#include <string.h>

namespace Knobs
{
    // Square-law gamma: LED brightness is linear in duty, the eye is not.
    static inline uint8_t gamma8(uint8_t v) { return (uint8_t)(((uint16_t)v * v + 127) / 255); }

    static inline uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t)
    {
        return (uint8_t)(a + (((int16_t)b - (int16_t)a) * t) / 255);
    }

    static inline Rgb scaled(Rgb c, uint8_t f)
    {
        c.r = (uint8_t)(((uint16_t)c.r * f) / 255);
        c.g = (uint8_t)(((uint16_t)c.g * f) / 255);
        c.b = (uint8_t)(((uint16_t)c.b * f) / 255);
        return c;
    }

    // The colour wheel at full saturation, 1536 steps round.
    static Rgb wheel(uint16_t h)
    {
        h = (uint16_t)(h % 1536);
        const uint8_t x = (uint8_t)(h & 0xFF);
        const uint8_t y = (uint8_t)(255 - x);
        switch (h >> 8) {
            case 0:  return Rgb{255, x, 0};
            case 1:  return Rgb{y, 255, 0};
            case 2:  return Rgb{0, 255, x};
            case 3:  return Rgb{0, y, 255};
            case 4:  return Rgb{x, 0, 255};
            default: return Rgb{255, 0, y};
        }
    }

    // Level of the unlit LEDs under Spin: dim, not dark, so the zone stays a backlight.
    static constexpr uint8_t SPIN_REST = 70;
    // One Sweep run: 150 ms over five LEDs = 30 ms/LED, about 1.5 frames at 50 Hz.
    static constexpr uint32_t SWEEP_MS = 150;
    // Bottom of a breath; never dark.
    static constexpr uint8_t BREATHE_MIN = 90;

    void Lights::configure(const Config& c)
    {
        const uint8_t oldSteps = _c.levelSteps;
        _c = c;
        _c.sanitize();
        _master = _c.brightness;
        _count = (uint8_t)(_c.zone[0].count + _c.zone[1].count + _c.feetCount);

        // Each zone's virtual position survives a settings change, rescaled, so a
        // colour-picker drag does not snap the gradient back. A pot overrides it via setLevel().
        for (uint8_t z = 0; z < ZONES; ++z) {
            ZoneState& s = _z[z];
            if (!s.seeded) {
                s.virt = (int16_t)(_c.levelSteps / 2);
                s.level = (uint8_t)((uint16_t)s.virt * 255 / _c.levelSteps);
                s.seeded = true;
            } else if (oldSteps && oldSteps != _c.levelSteps) {
                s.virt = (int16_t)((int32_t)s.virt * _c.levelSteps / oldSteps);
            }
            const uint8_t n = _c.zone[z].count;
            if (n && s.head >= n) s.head = (uint8_t)(s.head % n);
        }
    }

    void Lights::touch(uint8_t zone, uint32_t nowMs)
    {
        ZoneState& s = _z[zone];
        s.moved = true;
        s.lastMoveMs = nowMs;
        if (_c.zone[zone].react == React::Flash) s.flash = 255;
    }

    void Lights::onTurn(uint8_t zone, int8_t detents, uint32_t nowMs)
    {
        if (zone >= ZONES || !detents) return;
        ZoneState& s = _z[zone];

        int16_t v = (int16_t)(s.virt + detents);
        if (v < 0) v = 0;
        if (v > (int16_t)_c.levelSteps) v = (int16_t)_c.levelSteps;
        s.virt = v;
        s.level = (uint8_t)((uint16_t)v * 255 / _c.levelSteps);
        s.seeded = true;

        // Wraps: a ring has no end stop.
        const uint8_t n = _c.zone[zone].count;
        if (n) {
            int16_t h = (int16_t)((s.head + detents) % (int16_t)n);
            if (h < 0) h = (int16_t)(h + n);
            s.head = (uint8_t)h;
        }
        // Each click restarts the sweep, so a fast spin reads as a stream of dots.
        s.sweepAt  = nowMs ? nowMs : 1;
        s.sweepDir = detents > 0 ? 1 : -1;
        touch(zone, nowMs);
    }

    void Lights::setLevel(uint8_t zone, uint8_t level, uint32_t nowMs)
    {
        if (zone >= ZONES) return;
        ZoneState& s = _z[zone];
        const uint8_t n = _c.zone[zone].count;
        // A pot has end stops, so its lit LED is a position along the zone.
        const uint8_t head = n ? (uint8_t)(((uint16_t)level * n) >> 8) : 0;

        // First pot reading is a position, not a move. Separate flag because
        // configure() already sets `seeded` for the encoders.
        if (!s.levelSeeded) { s.levelSeeded = s.seeded = true; s.level = level; s.head = head; return; }
        if (level == s.level) return;
        s.level = level;
        s.head = head;
        touch(zone, nowMs);
    }

    void Lights::onButton(uint8_t zone, uint32_t nowMs)
    {
        if (zone >= ZONES || _c.zone[zone].react == React::None) return;
        // A press flashes under every reaction mode.
        _z[zone].flash = 255;
        (void)nowMs;
    }

    bool Lights::live(uint8_t zone, uint32_t nowMs) const
    {
        const ZoneState& s = _z[zone];
        // holdMs 0: the reaction IS the look, from the first frame on.
        return _c.reactHoldMs == 0 ||
               (s.moved && (uint32_t)(nowMs - s.lastMoveMs) < _c.reactHoldMs);
    }

    Rgb Lights::resting(uint8_t zone, uint8_t i, uint32_t nowMs) const
    {
        const Zone& zc = _c.zone[zone];
        const uint32_t period = (uint32_t)_c.animPeriod * 100u;

        switch (zc.anim) {
            case Anim::Breathe: {
                // Triangle through smoothstep: slow at both ends.
                const uint32_t p = (nowMs % period) * 512u / period;       // 0..511
                const uint32_t t = p < 256 ? p : 511 - p;                  // 0..255
                const uint32_t s = t * t * (765u - 2u * t) / 65025u;       // 0..255
                return scaled(zc.led[i], (uint8_t)(BREATHE_MIN + s * (255 - BREATHE_MIN) / 255));
            }
            case Anim::Rainbow: {
                const uint32_t base = (nowMs % period) * 1536u / period;
                const uint32_t off  = zc.count ? (uint32_t)i * 1536u / zc.count : 0;
                return wheel((uint16_t)((base + off) % 1536u));
            }
            default:
                return zc.led[i];
        }
    }

    Rgb Lights::target(uint8_t zone, uint8_t i, uint32_t nowMs) const
    {
        const Zone& zc = _c.zone[zone];
        const ZoneState& s = _z[zone];
        const Rgb rest = resting(zone, i, nowMs);

        switch (zc.react) {
            case React::Level: {
                if (!live(zone, nowMs)) return rest;
                Rgb c;
                c.r = lerp8(zc.lo.r, zc.hi.r, s.level);
                c.g = lerp8(zc.lo.g, zc.hi.g, s.level);
                c.b = lerp8(zc.lo.b, zc.hi.b, s.level);
                return c;
            }
            case React::Spin:
                if (!live(zone, nowMs)) return rest;
                // A single LED just shows `hi` while the knob moves.
                if (zc.count <= 1 || i == s.head) return zc.hi;
                return scaled(rest, SPIN_REST);
            case React::Sweep: {
                // Dot position is a pure function of time since the click, so a
                // skipped frame skips an LED instead of delaying the run.
                if (!s.sweepAt || zc.count == 0) return rest;
                const uint32_t t = nowMs - s.sweepAt;
                if (t >= SWEEP_MS) return rest;
                uint8_t k = (uint8_t)(t * zc.count / SWEEP_MS);
                if (s.sweepDir < 0) k = (uint8_t)(zc.count - 1 - k);
                return i == k ? zc.hi : scaled(rest, SPIN_REST);
            }
            case React::Trail: {
                if (!live(zone, nowMs) || zc.count == 0) return rest;
                if (zc.count <= 1 || i == s.head) return zc.hi;
                // d = distance behind the head along the turn direction; fades to SPIN_REST.
                const int16_t n = zc.count;
                int16_t d = s.sweepDir > 0 ? (int16_t)(s.head - i) : (int16_t)(i - s.head);
                if (d < 0) d = (int16_t)(d + n);
                if (d >= 3) return scaled(rest, SPIN_REST);
                const uint8_t f = (uint8_t)(SPIN_REST + (uint16_t)(255 - SPIN_REST) * (3 - d) / 4);
                Rgb c;
                c.r = lerp8(rest.r, zc.hi.r, f);
                c.g = lerp8(rest.g, zc.hi.g, f);
                c.b = lerp8(rest.b, zc.hi.b, f);
                return c;
            }
            default:
                return rest;
        }
    }

    static inline uint16_t ease(uint16_t cur, int32_t want)
    {
        const int32_t d = want - (int32_t)cur;
        // A quarter of the way per frame; snap the last bit or it never arrives.
        return (uint16_t)((d > -64 && d < 64) ? want : (int32_t)cur + d / 4);
    }

    // The footwells: `feetCount` LEDs starting at `out`, all one colour.
    void Lights::renderFeet(uint8_t* out, uint32_t nowMs)
    {
        const uint8_t n = _c.feetCount;
        if (!n) return;

        _feetCur[0] = ease(_feetCur[0], (int32_t)_c.feetColor.r << 8);
        _feetCur[1] = ease(_feetCur[1], (int32_t)_c.feetColor.g << 8);
        _feetCur[2] = ease(_feetCur[2], (int32_t)_c.feetColor.b << 8);
        _feetLevel  = ease(_feetLevel, _c.ledEnabled ? (int32_t)_c.feetBrightness << 8 : 0);

        // Own dimmer times master, both gamma'd.
        const uint16_t dim = (uint16_t)(((uint16_t)gamma8((uint8_t)(_feetLevel >> 8)) *
                                         gamma8(_master) + 127) / 255);
        if (!dim) return;                               // black is already there

        const uint32_t period = (uint32_t)_c.animPeriod * 100u;
        Rgb base{(uint8_t)(_feetCur[0] >> 8), (uint8_t)(_feetCur[1] >> 8), (uint8_t)(_feetCur[2] >> 8)};

        if (_c.feetAnim == Anim::Breathe) {
            const uint32_t p = (nowMs % period) * 512u / period;
            const uint32_t t = p < 256 ? p : 511 - p;
            const uint32_t s = t * t * (765u - 2u * t) / 65025u;
            base = scaled(base, (uint8_t)(BREATHE_MIN + s * (255 - BREATHE_MIN) / 255));
        }

        const uint32_t phase = (nowMs % period) * 1536u / period;
        for (uint8_t i = 0; i < n; ++i) {
            const Rgb c = _c.feetAnim == Anim::Rainbow
                        ? wheel((uint16_t)((phase + (uint32_t)i * 1536u / n) % 1536u))
                        : base;
            out[i * 3]     = (uint8_t)(((uint16_t)gamma8(c.r) * dim + 127) / 255);
            out[i * 3 + 1] = (uint8_t)(((uint16_t)gamma8(c.g) * dim + 127) / 255);
            out[i * 3 + 2] = (uint8_t)(((uint16_t)gamma8(c.b) * dim + 127) / 255);
        }
    }

    // WS2812B current is linear in the three duty bytes.
    uint32_t Lights::estimateMilliamps(const uint8_t* rgb, uint8_t count)
    {
        uint32_t sum = 0;
        for (uint16_t i = 0; i < (uint16_t)count * 3; ++i) sum += rgb[i];
        return (uint32_t)count * LED_MA_IDLE + sum * LED_MA_PER_CHANNEL / 255u;
    }

    // Must be the last step on a frame so nothing upstream can bypass it. One
    // factor for every byte, so hues do not shift.
    void Lights::limitCurrent(uint8_t* rgb)
    {
        const uint32_t want = estimateMilliamps(rgb, _count);
        _mAWanted = (uint16_t)(want > 65535u ? 65535u : want);
        _mA = _mAWanted;
        if (want <= _c.maxMilliamps) return;

        // Idle current is drawn regardless; only the rest can be scaled.
        const uint32_t idle   = (uint32_t)_count * LED_MA_IDLE;
        const uint32_t budget = _c.maxMilliamps > idle ? _c.maxMilliamps - idle : 0;
        const uint32_t lit    = want - idle;
        // Round down, or the limit can be exceeded.
        const uint32_t k = lit ? budget * 256u / lit : 0;               // 0..255
        for (uint16_t i = 0; i < (uint16_t)_count * 3; ++i)
            rgb[i] = (uint8_t)(((uint32_t)rgb[i] * k) >> 8);
        _mA = (uint16_t)estimateMilliamps(rgb, _count);
    }

    bool Lights::tick(uint32_t nowMs)
    {
        uint8_t* next = _nx;
        memset(next, 0, sizeof(_nx));
        const uint8_t gm = gamma8(_master);

        uint8_t at = 0;
        for (uint8_t k = 0; k < ZONES; ++k) {
            const uint8_t z = _c.rightFirst ? (uint8_t)(ZONES - 1 - k) : k;
            ZoneState& s = _z[z];

            for (uint8_t i = 0; i < _c.zone[z].count && at < MAX_LEDS; ++i, ++at) {
                const Rgb t = target(z, _c.ringReverse ? (uint8_t)(_c.zone[z].count - 1 - i) : i, nowMs);
                const uint8_t tv[3] = {t.r, t.g, t.b};
                for (uint8_t ch = 0; ch < 3; ++ch) {
                    const int32_t want = (int32_t)tv[ch] << 8;
                    if (!s.curSeeded) { s.cur[i][ch] = (uint16_t)want; continue; }
                    const int32_t d = want - (int32_t)s.cur[i][ch];
                    s.cur[i][ch] = (uint16_t)((d > -64 && d < 64) ? want
                                                                   : (int32_t)s.cur[i][ch] + d / 4);
                }

                if (!_c.ledEnabled) continue;           // eased, but stays black
                for (uint8_t ch = 0; ch < 3; ++ch) {
                    uint8_t v = (uint8_t)(s.cur[i][ch] >> 8);
                    if (s.flash) v = (uint8_t)(v + (((uint16_t)(255 - v) * s.flash) >> 9));
                    next[at * 3 + ch] = (uint8_t)(((uint16_t)gamma8(v) * gm + 127) / 255);
                }
            }
            s.curSeeded = true;
            if (s.flash) s.flash = (uint8_t)(((uint16_t)s.flash * 7) >> 3);
        }

        // Footwells are last on the chain (wired out of the bezel to the floor).
        renderFeet(next + (uint16_t)at * 3, nowMs);
        limitCurrent(next);

        if (memcmp(next, _px, sizeof(_px)) == 0) return false;
        memcpy(_px, next, sizeof(_px));
        return true;
    }
}
