#pragma once
#include <stdint.h>

// The Megane wheel is a THREE-PHASE cyclic encoder wired into the same keypad
// matrix, not a quadrature one: three matrix positions close in a repeating
// order, and the direction is which way round that order goes.
//
//   phase A -> B -> C -> A ...   one direction
//   phase A -> C -> B -> A ...   the other
//
// A step is the cyclic distance (mod 3) from the last closed phase: 1 is one
// direction, 2 the other. A working implementation for this keypad decodes
// '1'->'2'->'3' as seek-down, '1'->'3'->'2' as seek-up.
//
// Only a phase's rising edge is a step; a phase staying closed is a detent.
namespace Input
{
    class EncoderDecoder {
    public:
        static constexpr uint8_t NONE = 0xFF;

        // Matrix positions of phases A, B, C (0..8), NONE to disable.
        void setPhases(uint8_t a, uint8_t b, uint8_t c) { _p[0] = a; _p[1] = b; _p[2] = c; }
        void setInverted(bool inv) { _inverted = inv; }
        bool inverted() const { return _inverted; }

        bool isPhase(uint8_t pos) const { return phaseIndex(pos) != NONE; }

        // Feed the Down edge of a matrix position. +1 (clockwise / seek up),
        // -1 (counter-clockwise / seek down), 0 if not a phase or the first seen.
        int8_t onDown(uint8_t pos);

        void reset() { _last = NONE; }
        uint32_t steps() const { return _steps; }

    private:
        uint8_t phaseIndex(uint8_t pos) const {
            for (uint8_t i = 0; i < 3; ++i) if (_p[i] != NONE && _p[i] == pos) return i;
            return NONE;
        }

        uint8_t  _p[3]   = {NONE, NONE, NONE};
        uint8_t  _last   = NONE;
        bool     _inverted = false;
        uint32_t _steps  = 0;
    };
}
