#include "VehicleState.h"

namespace Vehicle
{
    int32_t State::clamp(uint8_t s, int32_t v)
    {
        const Meta& m = meta(s);
        if (v < m.min) return m.min;
        if (v > m.max) return m.max;
        return v;
    }

    void State::set(uint8_t s, int32_t v, uint32_t nowMs)
    {
        if (s >= SIG_COUNT) return;
        if (!_pinned[s] && (!_ts[s] || _val[s] != v)) _changed.set(s);
        _val[s] = v;
        _ts[s]  = nowMs ? nowMs : 1;    // 0 stays the "never decoded" sentinel
    }

    void State::clear(uint8_t s)
    {
        if (s >= SIG_COUNT || !_ts[s]) return;
        _ts[s] = 0;
        if (!_pinned[s]) _changed.set(s);
    }

    void State::pin(uint8_t s, int32_t v)
    {
        if (s >= SIG_COUNT) return;
        const int32_t c = clamp(s, v);
        if (!_pinned[s] || _pin[s] != c) _changed.set(s);
        _pin[s]    = c;
        _pinned[s] = true;
    }

    void State::unpin(uint8_t s)
    {
        if (s >= SIG_COUNT || !_pinned[s]) return;
        _pinned[s] = false;
        _changed.set(s);
    }
}
