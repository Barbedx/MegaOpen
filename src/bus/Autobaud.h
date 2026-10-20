#pragma once
#include <stdint.h>

// Which bit rate the car's bus runs at, found by listening (docs/design/event-
// tasks.md, "5b additions": the bus configures itself). Portable, no driver:
// the controller is the caller's, this only decides. Native-tested
// (test/test_car).
//
// The rule, as the owner gave it:
//   * LISTEN-ONLY, always. A wrong rate in normal mode would put error frames
//     on a car's bus; listen-only never drives it, so trying is harmless.
//   * Try 500k, 250k, 125k, 1M, in that order, and keep the FIRST rate that
//     gives frames with no error frames.
//   * A saved rate (the last one that locked, from NVS) is tried first.
//   * A rate set by hand in the settings always wins: only that rate, never
//     cycled, whatever the bus says.
//
// A rate is rejected the moment the controller reports a receive error (CRC,
// stuffing, form): at the wrong rate a busy bus produces those within a frame
// or two. It is accepted at LOCK_FRAMES clean frames, or at the end of its dwell
// with at least one clean frame and still no error (a sparse bus). A rate with
// no traffic and no error moves on after its dwell, so with no bus at all the
// four rates go round slowly for ever; `cycles()` says how many rounds.
//
// Once locked it stays locked until reboot: a car that goes to sleep is a quiet
// bus at the right rate, not a reason to search again.
namespace Bus
{
    class Autobaud {
    public:
        static constexpr uint8_t  N_RATES = 4;
        static constexpr uint32_t DEFAULT_DWELL_MS = 1500;
        static constexpr uint32_t LOCK_FRAMES = 4;

        // The search order without a saved rate.
        static uint32_t rateAt(uint8_t i);
        static bool     valid(uint32_t bps);

        enum class State : uint8_t { Searching = 0, Locked = 1, Manual = 2 };
        enum class Step  : uint8_t { Stay, Lock, Next };

        // manualBps: a valid rate fixes it (State::Manual); 0 or invalid = auto.
        // savedBps: a valid rate goes first in the order. nowMs starts the
        // first dwell; the caller starts the controller on rate().
        void begin(uint32_t manualBps, uint32_t savedBps, uint32_t nowMs,
                   uint32_t dwellMs = DEFAULT_DWELL_MS);

        // What the controller has seen on rate() since it started on it,
        // cumulative (frames that passed CRC; receive errors of any kind).
        // Next: the caller restarts the controller on the new rate() and
        // calls started().
        Step observe(uint32_t nowMs, uint32_t frames, uint32_t errors);
        void started(uint32_t nowMs) { _startedMs = nowMs; }

        uint32_t rate() const   { return _order[_i]; }
        State    state() const  { return _state; }
        bool     locked() const { return _state == State::Locked; }
        // The rate that locked (0 while searching); a manual rate is not "detected".
        uint32_t detected() const { return _state == State::Locked ? rate() : 0; }
        // Full rounds of the order that ended with no lock.
        uint32_t cycles() const { return _cycles; }
        // Rates given up because they showed errors.
        uint32_t rejected() const { return _rejected; }

    private:
        void next();

        uint32_t _order[N_RATES] = {};
        uint8_t  _n = 0;
        uint8_t  _i = 0;
        State    _state = State::Searching;
        uint32_t _dwellMs = DEFAULT_DWELL_MS;
        uint32_t _startedMs = 0;
        uint32_t _cycles = 0;
        uint32_t _rejected = 0;
    };
}
