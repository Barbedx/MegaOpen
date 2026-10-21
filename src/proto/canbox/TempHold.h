#pragma once
#include <stdint.h>

// The outside temperature the head unit is shown, held against jitter.
//
// 0x28 is the head unit's climate frame, and any change in it may pop the
// climate overlay (docs/hu-decoder.md §2d: every A/C code does; the DUDU
// launcher, which takes over climate display on this unit, is not readable).
// Our 0x28 carries only the outside temperature (D5), from a hypothesis row,
// so a value flickering between two degrees would change the frame again and
// again. A new value is shown only once it has been the reading for HOLD_MS
// without a break; the first reading is shown at once.
//
// Pure: host-tested in test_vehicle.
namespace Canbox
{
    struct TempHold {
        static constexpr uint32_t HOLD_MS = 60000;

        // The value to send for `v` read at `nowMs`.
        int32_t step(int32_t v, uint32_t nowMs)
        {
            if (!_have) { _have = true; _shown = _cand = v; _since = nowMs; return v; }
            if (v == _shown) { _cand = v; return _shown; }
            if (v != _cand) { _cand = v; _since = nowMs; return _shown; }
            if ((uint32_t)(nowMs - _since) >= HOLD_MS) _shown = v;
            return _shown;
        }
        bool    have() const  { return _have; }
        int32_t shown() const { return _shown; }

    private:
        bool     _have = false;
        int32_t  _shown = 0, _cand = 0;
        uint32_t _since = 0;
    };
}
