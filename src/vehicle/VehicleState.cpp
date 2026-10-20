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
        _val[s] = v;
        _ts[s]  = nowMs ? nowMs : 1;    // 0 stays the "never decoded" sentinel
    }

    void State::pin(uint8_t s, int32_t v)
    {
        if (s >= SIG_COUNT) return;
        _pin[s]    = clamp(s, v);
        _pinned[s] = true;
    }

    void State::unpin(uint8_t s)
    {
        if (s < SIG_COUNT) _pinned[s] = false;
    }
}
