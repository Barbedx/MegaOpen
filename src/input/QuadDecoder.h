#pragma once
#include <stdint.h>

// Two-contact quadrature encoder (EC11 etc.); not EncoderDecoder, which decodes
// the Megane wheel's three-phase ring from the matrix. Portable, native-tested.
//
// 4x4 transition table: one contact changing = a quarter step; both at once =
// a missed sample, counted as an error and credited as two quarter steps the
// way the knob was last going (no direction known: dropped). Bounce cancels
// itself, no debounce.
// Quarter steps are accumulated into detents (see setPulsesPerDetent).
namespace Input
{
    class QuadDecoder {
    public:
        // Quarter steps per mechanical click: 4 (EC11), 2 (half-cycle parts) or 1
        // (report every edge — for a part with no detents at all).
        void setPulsesPerDetent(uint8_t n) { _ppd = (n == 1 || n == 2) ? n : 4; _acc = 0; }
        uint8_t pulsesPerDetent() const { return _ppd; }

        void setInverted(bool inv) { _inverted = inv; }
        bool inverted() const { return _inverted; }

        // One sample of both contacts (polarity just has to match).
        // Returns +1 (CW), -1 (CCW) or 0.
        int8_t update(bool a, bool b, uint32_t nowMs);

        void reset() { _seeded = false; _acc = 0; _lastQ = 0; }

        uint32_t detents() const { return _detents; }
        // Two-contact jumps. A few while spinning fast is normal; a steady
        // stream at rest means a floating input.
        uint32_t errors() const { return _errors; }

    private:
        // After this long at rest the accumulator is re-zeroed (the knob sits on
        // a detent), so one missed quarter step cannot shift phase permanently.
        static constexpr uint32_t REST_MS = 200;

        uint8_t  _ppd = 4;
        bool     _inverted = false;
        bool     _seeded = false;
        uint8_t  _state = 0;
        int8_t   _acc = 0;
        // Sign of the last valid quarter step; 0 after a reset or a rest. The
        // direction a two-contact jump is credited to.
        int8_t   _lastQ = 0;
        uint32_t _lastMoveMs = 0;
        uint32_t _detents = 0;
        uint32_t _errors = 0;
    };
}
