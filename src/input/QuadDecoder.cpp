#include "QuadDecoder.h"

namespace Input
{
    // Indexed by (previous << 2) | current, each a two-bit (a << 1) | b state.
    // +1 / -1 are quarter steps; 2 marks the jumps where both contacts changed.
    // A LEADING B IS CLOCKWISE (00 -> 10 -> 11 -> 01), which is how an EC11 module
    // labels S1/S2. A part wired the other way round is a checkbox, not a resolder.
    static const int8_t kQuarter[16] = {
         0, -1, +1,  2,
        +1,  0,  2, -1,
        -1,  2,  0, +1,
         2, +1, -1,  0,
    };

    int8_t QuadDecoder::update(bool a, bool b, uint32_t nowMs)
    {
        const uint8_t s = (uint8_t)((a ? 2 : 0) | (b ? 1 : 0));

        if (!_seeded) { _seeded = true; _state = s; _lastMoveMs = nowMs; return 0; }

        if (s == _state) {
            if ((uint32_t)(nowMs - _lastMoveMs) >= REST_MS) { _acc = 0; _lastQ = 0; }
            return 0;
        }

        const int8_t q = kQuarter[(_state << 2) | s];
        _state = s;
        _lastMoveMs = nowMs;

        if (q == 2) {
            // One sample missed mid-turn (a late tick, a failed expander read):
            // the knob went on the way it was going. Dropping the two quarters
            // instead would leave this detent half a click short. With no
            // direction yet there is nothing to go on: drop them, as before.
            ++_errors;
            if (!_lastQ) return 0;
            _acc = (int8_t)(_acc + 2 * _lastQ);
        } else {
            _lastQ = q;
            _acc = (int8_t)(_acc + q);
        }
        if (_acc < (int8_t)_ppd && _acc > -(int8_t)_ppd) return 0;

        const bool cw = _acc > 0;
        _acc = 0;
        ++_detents;
        return (int8_t)((cw != _inverted) ? +1 : -1);
    }
}
