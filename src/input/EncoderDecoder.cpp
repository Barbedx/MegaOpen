#include "EncoderDecoder.h"

namespace Input
{
    int8_t EncoderDecoder::onDown(uint8_t pos)
    {
        const uint8_t idx = phaseIndex(pos);
        if (idx == NONE) return 0;

        if (_last == NONE) { _last = idx; return 0; }   // seed, no step to report
        if (idx == _last)  { return 0; }                // re-press of the same phase

        // Cyclic distance: 1 one way round the A->B->C ring, 2 the other. With
        // three phases every change is a valid step — there is no error case to
        // reject, which is exactly why this encoder is robust at speed.
        const uint8_t delta = (uint8_t)((idx + 3 - _last) % 3);
        _last = idx;
        ++_steps;

        const bool cw = (delta == 2);
        return (int8_t)((cw != _inverted) ? +1 : -1);
    }
}
