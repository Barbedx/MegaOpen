#pragma once
#include <stdint.h>

// 3x3 keypad scan over the 6 wires of the OEM display connector
// (ARCHITECTURE.md §4). GPIO and settle delay are callbacks, so it runs in
// native tests. Position = row*COLS + col; KeyMap owns what a position means.
namespace Input
{
    class MatrixScanner {
    public:
        static constexpr uint8_t ROWS = 3;
        static constexpr uint8_t COLS = 3;
        static constexpr uint8_t POSITIONS = ROWS * COLS;

        // active=true pulls the row LOW, false releases it to Hi-Z. Never drive
        // a row high: two keys at once would short it to a low row.
        using DriveFn = void (*)(uint8_t row, bool active, void* ctx);
        // Bit per column, 1 = closed (pin LOW; columns are pulled up).
        using ReadFn  = uint8_t (*)(void* ctx);
        // wait(): let the line settle after switching a row. May be null.
        using WaitFn  = void (*)(void* ctx);

        void begin(DriveFn drive, ReadFn read, WaitFn wait, void* ctx) {
            _drive = drive; _read = read; _wait = wait; _ctx = ctx;
        }

        // One sweep; returns the 9-bit bitmap. A ghosted sweep returns the
        // previous bitmap and bumps ghosts(), never "everything released".
        uint16_t scan();

        // No diodes: two rows sharing two or more closed columns is ambiguous.
        // Sharing one column is allowed (encoder row closed while a key is held).
        static bool isGhosted(const uint8_t rowBits[ROWS]);

        uint16_t last() const { return _last; }
        uint32_t ghosts() const { return _ghosts; }

    private:
        DriveFn  _drive = nullptr;
        ReadFn   _read  = nullptr;
        WaitFn   _wait  = nullptr;
        void*    _ctx   = nullptr;
        uint16_t _last  = 0;
        uint32_t _ghosts = 0;
    };
}
