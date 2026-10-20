#pragma once
#include <stdint.h>

// Per-position debounce + long-press + auto-repeat over the raw 9-bit matrix
// bitmap (ARCHITECTURE.md §4). Portable; time arrives as nowMs.
// Buttons need ~20 ms settling; the three encoder phases need ~2 ms or fast
// spinning loses detents. fastMask positions take the short path, no Long/Repeat.
namespace Input
{
    enum class Edge : uint8_t { Down = 0, Up = 1, Long = 2, Repeat = 3 };

    struct PosEvent {
        uint8_t pos;    // 0..8, row*3+col
        Edge    edge;
        uint32_t tMs;
    };

    class KeyDebouncer {
    public:
        static constexpr uint8_t POSITIONS = 9;
        static constexpr uint8_t MAX_EVENTS = 12;

        struct Params {
            uint16_t debounceMs     = 20;    // normal keys
            uint16_t fastDebounceMs = 2;     // encoder phases
            uint16_t longMs         = 600;   // press -> Long
            uint16_t repeatMs       = 150;   // Long -> Repeat, Repeat -> Repeat
            bool     repeatEnabled  = true;
        };

        void configure(const Params& p, uint16_t fastMask) { _p = p; _fast = fastMask; }
        const Params& params() const { return _p; }

        // Feed one raw sweep; returns how many events were written to out.
        uint8_t update(uint16_t raw, uint32_t nowMs, PosEvent* out, uint8_t maxOut);

        uint16_t stable() const { return _stable; }
        void reset();

    private:
        struct Pos {
            bool     stable   = false;   // debounced state
            bool     candidate= false;   // what raw has been showing
            uint32_t sinceMs  = 0;       // when raw last changed
            uint32_t downMs   = 0;       // when the debounced press started
            bool     longSent = false;
            uint32_t nextRepeatMs = 0;
        };

        Params   _p;
        uint16_t _fast   = 0;
        uint16_t _stable = 0;
        Pos      _pos[POSITIONS];
    };
}
