#include "CanDecode.h"

namespace Vehicle
{
    namespace
    {
        // The compiled table: docs/megane-can-ids.md, sorted by id.
        const DecodeRow kDefaults[] = {
#include "decode_defaults.inc"
        };
        constexpr uint8_t kDefaultCount = (uint8_t)(sizeof(kDefaults) / sizeof(kDefaults[0]));
        static_assert(sizeof(DecodeRow) == 24, "DecodeRow grew: DecodeTable holds 2 x 48 of them");
        static_assert(sizeof(kDefaults) / sizeof(kDefaults[0]) <= CanDecode::MAX_ROWS,
                      "decode_defaults.inc has more rows than CanDecode::MAX_ROWS");

        // 11-bit and 29-bit ids are different keys: 0x123 standard is not
        // 0x00000123 extended.
        inline uint32_t key(uint32_t id, bool ext) { return ext ? (id | 0x80000000u) : id; }
        inline uint32_t key(const DecodeRow& r) { return key(r.canId, (r.flags & DF_EXT) != 0); }
    }

    const DecodeRow* CanDecode::defaults(uint8_t& n)
    {
        n = kDefaultCount;
        return kDefaults;
    }

    bool CanDecode::loadDefaults()
    {
        return load(kDefaults, kDefaultCount);
    }

    bool CanDecode::load(const DecodeRow* rows, uint8_t n)
    {
        if (n > MAX_ROWS || (n && !rows)) return false;
        for (uint8_t i = 0; i < n; ++i) {
            const DecodeRow& r = rows[i];
            if (r.sig >= SIG_COUNT) return false;
            if (r.numBytes < 1 || r.numBytes > 4 || r.startByte + r.numBytes > 8) return false;
            if (r.shift >= 32 || !r.mask) return false;
            // The search below relies on it, and an unsorted table would
            // silently lose rows rather than fail.
            if (i && key(rows[i - 1]) > key(r)) return false;
        }
        _r = rows;
        _n = n;
        for (uint8_t i = 0; i < MAX_ROWS; ++i) { _until[i] = 0; _held[i] = 0; }
        return true;
    }

    uint8_t CanDecode::lowerBound(uint32_t k) const
    {
        uint8_t lo = 0, hi = _n;
        while (lo < hi) {
            const uint8_t mid = (uint8_t)((lo + hi) / 2);
            if (key(_r[mid]) < k) lo = (uint8_t)(mid + 1);
            else                  hi = mid;
        }
        return lo;
    }

    bool CanDecode::extract(const DecodeRow& r, const uint8_t* data, uint8_t len, int32_t& out)
    {
        if (r.numBytes < 1 || r.numBytes > 4 || r.startByte + r.numBytes > len) return false;
        uint32_t raw = 0;
        if (r.flags & DF_LE) {
            for (int8_t i = (int8_t)(r.numBytes - 1); i >= 0; --i)
                raw = (raw << 8) | data[r.startByte + i];
        } else {
            for (uint8_t i = 0; i < r.numBytes; ++i)
                raw = (raw << 8) | data[r.startByte + i];
        }
        const uint32_t field = (raw >> r.shift) & r.mask;
        int64_t v = field;
        if (r.flags & DF_SIGNED) {
            // The sign bit is the mask's top bit: a 16-bit mask is int16.
            uint32_t top = r.mask;
            top |= top >> 1; top |= top >> 2; top |= top >> 4; top |= top >> 8; top |= top >> 16;
            top = (top >> 1) + 1;
            if (field & top) v -= (int64_t)r.mask + 1;
        }
        const int64_t div = r.div ? r.div : 1;
        v = v * r.mul / div + r.add;
        if (v > INT32_MAX) v = INT32_MAX;
        if (v < INT32_MIN) v = INT32_MIN;
        out = (int32_t)v;
        return true;
    }

    uint8_t CanDecode::apply(const Frame& f, State& s, uint32_t nowMs)
    {
        const uint32_t k = key(f.id, f.extended);
        uint8_t i = lowerBound(k);
        uint8_t matched = 0;
        const uint8_t len = f.len > 8 ? 8 : f.len;
        for (; i < _n && key(_r[i]) == k; ++i) {
            const DecodeRow& r = _r[i];
            ++matched;
            int32_t v;
            if (!extract(r, f.data, len, v)) { ++_short; continue; }

            if (r.flags & DF_KEY) {
                // A key is an edge, not a level: only a change is news.
                if (v != _held[i]) {
                    _held[i] = v;
                    if (_key) _key(r.sig, v, _keyCtx);
                }
                continue;
            }
            v = State::clamp(r.sig, v);
            if (r.flags & DF_LATCH) {
                if (v != 0) {
                    _held[i]  = v;
                    _until[i] = nowMs + r.holdMs;
                } else if ((int32_t)(_until[i] - nowMs) > 0) {
                    v = _held[i];      // between two blinks: still on
                }
            }
            s.set(r.sig, v, nowMs);
        }
        if (matched) ++_hits;
        else         ++_unknown;
        return matched;
    }
}
