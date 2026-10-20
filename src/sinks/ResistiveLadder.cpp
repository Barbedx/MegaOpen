#include "ResistiveLadder.h"

namespace Sinks
{
    void ResistiveLadder::setLeg(uint8_t i, uint16_t ohms)
    {
        if (i >= MAX_LEGS) return;
        // Clamped, not rejected, so the page shows the value actually stored.
        if (ohms && ohms < MIN_OHMS) ohms = MIN_OHMS;
        if (ohms > MAX_OHMS)         ohms = MAX_OHMS;
        _leg[i] = ohms;
    }

    bool ResistiveLadder::inUsableWindow() const
    {
        for (uint8_t i = 0; i < _n; ++i) {
            if (!_leg[i]) continue;                     // not populated, not a fault
            const uint16_t r = (uint16_t)(_leg[i] + _series);
            if (r < USABLE_MIN_OHMS || r > USABLE_MAX_OHMS) return false;
        }
        return _n != 0;
    }

    uint16_t ResistiveLadder::ohmsFor(uint8_t mask) const
    {
        if (!_n) return OPEN;
        uint32_t sum = 0;
        for (uint8_t i = 0; i < _n; ++i)
            if ((mask & (1u << i)) && _leg[i]) sum += nS(_leg[i]);

        // No leg engaged, or only unpopulated ones: open line, not zero ohms.
        if (!sum) return OPEN;

        uint32_t r = 1000000000UL / sum;
        r += _series;
        return r > MAX_OHMS ? MAX_OHMS : (uint16_t)r;
    }

    uint8_t ResistiveLadder::chooseMasks(uint8_t count, uint8_t* out) const
    {
        if (!count || !out || !_n) return 0;

        const uint8_t levels = levelCount();

        // Every reachable level, insertion-sorted by resistance (<= 31 entries,
        // run only when a setting changes).
        uint8_t  masks[(1u << MAX_LEGS)];
        uint16_t ohms [(1u << MAX_LEGS)];
        uint8_t  n = 0;
        for (uint8_t m = 1; m <= levels; ++m) {
            const uint16_t r = ohmsFor(m);
            if (r == OPEN) continue;                    // legs not populated
            // Rungs outside the usable window are never offered (all five
            // default legs in parallel reach 90 Ω).
            if (r < USABLE_MIN_OHMS || r > USABLE_MAX_OHMS) continue;
            uint8_t j = n++;
            while (j && ohms[j - 1] > r) { ohms[j] = ohms[j - 1]; masks[j] = masks[j - 1]; --j; }
            ohms[j] = r; masks[j] = m;
        }
        if (!n) return 0;
        if (count >= n) {                                // asked for everything
            for (uint8_t i = 0; i < n; ++i) out[i] = masks[i];
            return n;
        }

        // ---- one leg per button first ----
        // Parallel combinations bunch (560/1500/3900/10000 puts 8 of 15 levels
        // between 356 and 560 Ω); single-leg rungs are the widest spread.
        if (count <= _n) {
            uint8_t placed = 0;
            for (uint8_t i = 0; i < n && placed < count; ++i) {
                const uint8_t m = masks[i];
                if (m & (m - 1)) continue;               // more than one bit set
                out[placed++] = m;
            }
            if (placed == count) return placed;
            // Some legs unpopulated: fall through to the general spread from
            // scratch rather than topping up the partial result.
        }

        // ---- maximise the worst ratio ----
        // Resistance separation is a ratio, so binary-search the largest % gap
        // at which `count` rungs still fit, then place greedily from the lowest
        // (optimal for max-min spacing).
        auto place = [&](uint16_t pct, uint8_t* dst) -> uint8_t {
            uint8_t k = 0;
            uint32_t floorOhms = 0;
            for (uint8_t i = 0; i < n && k < count; ++i) {
                if (ohms[i] < floorOhms) continue;
                if (dst) dst[k] = masks[i];
                ++k;
                floorOhms = (uint32_t)ohms[i] * (100u + pct) / 100u;
                // Guarantee forward progress when the step rounds to zero.
                if (floorOhms <= ohms[i]) floorOhms = (uint32_t)ohms[i] + 1;
            }
            return k;
        };

        uint16_t lo = 0, hi = 400, best = 0;
        while (lo <= hi) {
            const uint16_t mid = (uint16_t)((lo + hi) / 2);
            if (place(mid, nullptr) >= count) { best = mid; lo = (uint16_t)(mid + 1); }
            else                              { if (!mid) break; hi = (uint16_t)(mid - 1); }
        }
        // Output is ascending, since `place` walks the sorted array.
        return place(best, out);
    }

    uint8_t ResistiveLadder::worstSeparationPct(const uint8_t* masks, uint8_t n) const
    {
        // 255 means "at least 255%" (single-leg rungs sit ~156-167% apart).
        if (!masks || n < 2) return 255;      // nothing to collide with
        uint32_t worst = 255;
        for (uint8_t i = 0; i < n; ++i) {
            const uint16_t a = ohmsFor(masks[i]);
            if (a == OPEN || !masks[i]) continue;
            for (uint8_t j = (uint8_t)(i + 1); j < n; ++j) {
                const uint16_t b = ohmsFor(masks[j]);
                if (b == OPEN || !masks[j]) continue;
                // Same rung = collision.
                if (a == b) return 0;
                const uint16_t lo = a < b ? a : b, hi = a < b ? b : a;
                const uint32_t pct = (uint32_t)(hi - lo) * 100u / lo;
                if (pct < worst) worst = pct;
            }
        }
        return worst > 255 ? 255 : (uint8_t)worst;
    }
}
