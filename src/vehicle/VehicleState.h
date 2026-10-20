#pragma once
#include "Signals.h"

// One value per signal, plus where it came from:
//   * unknown      — never decoded, not pinned: the emitter stays silent about it;
//   * decoded      — a CAN frame set it, with a timestamp so staleness is visible;
//   * pinned       — a value typed in the web UI, which WINS over CAN.
// Pinning lets the head-unit side be tested without any decoded CAN.
namespace Vehicle
{
    class State {
    public:
        // --- from the decoder -------------------------------------------------
        void set(uint8_t s, int32_t v, uint32_t nowMs);

        // --- from the web UI --------------------------------------------------
        void pin(uint8_t s, int32_t v);      // clamped to the signal's range
        void unpin(uint8_t s);
        bool pinned(uint8_t s) const { return s < SIG_COUNT && _pinned[s]; }

        // --- consumers --------------------------------------------------------
        // has() is what the emitter asks: is there anything worth sending?
        bool has(uint8_t s) const { return s < SIG_COUNT && (_pinned[s] || _ts[s] != 0); }
        bool fresh(uint8_t s, uint32_t nowMs, uint32_t maxAgeMs) const {
            if (s >= SIG_COUNT) return false;
            if (_pinned[s]) return true;                 // a pinned value never goes stale
            return _ts[s] != 0 && (uint32_t)(nowMs - _ts[s]) <= maxAgeMs;
        }
        int32_t get(uint8_t s, int32_t fallback = 0) const {
            if (s >= SIG_COUNT) return fallback;
            if (_pinned[s]) return _pin[s];
            return _ts[s] ? _val[s] : fallback;
        }
        // Last decoded value regardless of any pin (the UI shows both).
        int32_t decoded(uint8_t s) const { return s < SIG_COUNT ? _val[s] : 0; }
        uint32_t stamp(uint8_t s) const { return s < SIG_COUNT ? _ts[s] : 0; }
        // Since CAN last set it: 0 while pinned (a pin is always current),
        // NEVER when nothing has set it.
        static constexpr uint32_t NEVER = UINT32_MAX;
        uint32_t ageMs(uint8_t s, uint32_t nowMs) const {
            if (s >= SIG_COUNT) return NEVER;
            if (_pinned[s]) return 0;
            return _ts[s] ? (uint32_t)(nowMs - _ts[s]) : NEVER;
        }

        static int32_t clamp(uint8_t s, int32_t v);

    private:
        int32_t  _val[SIG_COUNT] = {};
        uint32_t _ts[SIG_COUNT]  = {};    // 0 = never decoded
        int32_t  _pin[SIG_COUNT] = {};
        bool     _pinned[SIG_COUNT] = {};
    };
}
