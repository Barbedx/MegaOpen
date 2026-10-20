#include "MatrixScanner.h"

namespace Input
{
    static inline uint8_t popcount3(uint8_t v) {
        return (uint8_t)((v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1));
    }

    bool MatrixScanner::isGhosted(const uint8_t rowBits[ROWS])
    {
        for (uint8_t i = 0; i < ROWS; ++i) {
            for (uint8_t j = (uint8_t)(i + 1); j < ROWS; ++j) {
                if (popcount3((uint8_t)(rowBits[i] & rowBits[j])) >= 2) return true;
            }
        }
        return false;
    }

    uint16_t MatrixScanner::scan()
    {
        if (!_drive || !_read) return _last;

        uint8_t rowBits[ROWS];
        for (uint8_t r = 0; r < ROWS; ++r) {
            _drive(r, true, _ctx);
            if (_wait) _wait(_ctx);
            rowBits[r] = (uint8_t)(_read(_ctx) & 0x07);
            _drive(r, false, _ctx);
        }

        if (isGhosted(rowBits)) { ++_ghosts; return _last; }

        uint16_t map = 0;
        for (uint8_t r = 0; r < ROWS; ++r)
            map |= (uint16_t)(rowBits[r] & 0x07) << (r * COLS);

        _last = map;
        return map;
    }
}
