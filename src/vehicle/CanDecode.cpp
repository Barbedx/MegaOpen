#include "CanDecode.h"

namespace Vehicle
{
    namespace
    {
        // The compiled table: docs/megane-can-ids.md, sorted by id.
        const DecodeRow kDefaults[] = {
#include "decode_defaults.inc"
        };
        constexpr uint16_t kDefaultCount = (uint16_t)(sizeof(kDefaults) / sizeof(kDefaults[0]));
        static_assert(sizeof(DecodeRow) == 24, "DecodeRow grew: the staged table and CanDecode each hold a copy");
        static_assert(sizeof(kDefaults) / sizeof(kDefaults[0]) <= CanDecode::MAX_ROWS,
                      "decode_defaults.inc has more rows than CanDecode::MAX_ROWS");

        // 11-bit and 29-bit ids are different keys: 0x123 standard is not
        // 0x00000123 extended.
        inline uint32_t key(uint32_t id, bool ext) { return ext ? (id | 0x80000000u) : id; }
        inline uint32_t key(const DecodeRow& r) { return key(r.canId, (r.flags & DF_EXT) != 0); }
    }

    const DecodeRow* CanDecode::defaults(uint16_t& n)
    {
        n = kDefaultCount;
        return kDefaults;
    }

    bool CanDecode::loadDefaults()
    {
        return load(kDefaults, kDefaultCount);
    }

    bool CanDecode::load(const DecodeRow* rows, uint16_t n)
    {
        if (n > MAX_ROWS || (n && !rows)) return false;
        for (uint16_t i = 0; i < n; ++i) {
            const DecodeRow& r = rows[i];
            if (r.sig >= SIG_COUNT) return false;
            if (r.numBytes < 1 || r.numBytes > 4 || r.startByte + r.numBytes > 8) return false;
            if (r.shift >= 32 || !r.mask) return false;
            // The search below relies on it, and an unsorted table would
            // silently lose rows rather than fail.
            if (i && key(rows[i - 1]) > key(r)) return false;
        }
        if (!_rows.assign(rows, n) || !_st.resize(n)) {
            _rows.clear();
            _r = nullptr;
            _n = 0;
            return false;
        }
        _r = _rows.data();
        _n = n;
        return true;
    }

    uint16_t CanDecode::lowerBound(uint32_t k) const
    {
        uint16_t lo = 0, hi = _n;
        while (lo < hi) {
            const uint16_t mid = (uint16_t)((lo + hi) / 2);
            if (key(_r[mid]) < k) lo = (uint16_t)(mid + 1);
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
        uint16_t i = lowerBound(k);
        uint8_t matched = 0;
        const uint8_t len = f.len > 8 ? 8 : f.len;
        for (; i < _n && key(_r[i]) == k; ++i) {
            const DecodeRow& r = _r[i];
            ++matched;
            int32_t v;
            if (!extract(r, f.data, len, v)) { ++_short; continue; }
            // Before the clamp, which would read 11 as a boolean's 1.
            if (r.flags & DF_STATE2) {
                if (v < 0 || v > 1) { ++_invalid; continue; }
                if (r.flags & DF_STATE2_INV) v = !v;
            }

            if (r.flags & DF_KEY) {
                // A key is an edge, not a level: only a change is news.
                if (v != _st[i].held) {
                    _st[i].held = v;
                    if (_key) _key(r.sig, v, _keyCtx);
                }
                continue;
            }
            v = State::clamp(r.sig, v);
            if (r.flags & DF_LATCH) {
                if (v != 0) {
                    _st[i].held  = v;
                    _st[i].until = nowMs + r.holdMs;
                } else if ((int32_t)(_st[i].until - nowMs) > 0) {
                    v = _st[i].held;      // between two blinks: still on
                }
            }
            s.set(r.sig, v, nowMs);
        }
        if (matched) ++_hits;
        else         ++_unknown;
        return matched;
    }
}
