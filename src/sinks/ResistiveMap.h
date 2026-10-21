#pragma once
#include <stdint.h>
#include "ResistiveLadder.h"
#include "../keys/KeyAction.h"

// Resistive-path configuration, persisted as one blob: the two ladders (fitted
// resistors) and, per logical action, which channel and rung it presses.
//
// Stored as a switch mask, not a resistance: re-stuffing a leg then moves the
// rung's ohms but every button keeps pressing the same physical switches.
// Per-action, not per-matrix-position, because the encoder (EncCw/EncCcw) has
// no matrix position of its own.
// Two channels: DUDU7 main connector G7 ("вход 1") and G6 ("вход 2"), G1 ground.
namespace Sinks
{
    class ResistiveMap {
    public:
        static constexpr uint8_t CHANNELS = 2;
        static constexpr uint8_t ACTIONS  = (uint8_t)Keys::Action::_Count;

        // `mask` 0 = unassigned (the default; the sink is silent until set up).
        // `longMask`: a long hold presses a different rung. It costs latency on
        // that button — nothing goes on the wire until the long-press threshold
        // decides short vs long — so it is per-action and off by default.
        struct Slot {
            uint8_t ch       = 0;
            uint8_t mask     = 0;
            uint8_t longMask = 0;
        };

        // How encoder detents become presses. A setting, not a constant: the
        // right choice depends on the head unit's undocumented key repeat.
        enum class Spin : uint8_t {
            // One fixed press per detent; ~9 detents/s at the defaults
            // (1000/(pulse+gap)). A fast spin queues and lags.
            Pulse = 0,
            // Same, gap shrinks as the queue deepens (pulse does not; see
            // minPulseMs). Buys perhaps 30%.
            Compress = 1,
            // Pulse below `holdAfter`; past it, hold the rung and let the head
            // unit's key repeat deliver the rest. See startNextDetent().
            Hybrid = 2,
        };
        // Spin's rows for /api/catalog (enums.resSpin), in enum order.
        static constexpr const char* SPIN_TITLES[] = {nullptr, nullptr, nullptr};

        struct Timing {
            // 50 ms is what a shipping digipot adapter (bigevtaylor/arduino-swc)
            // and an ATtiny build both use; shorter was not recognised. +margin.
            uint16_t pulseMs    = 60;
            // Inferred, unmeasured: the gap must clear the unit's ADC scan and
            // debounce so it sees the idle level between presses.
            uint16_t gapMs      = 50;
            // Hard floor: below 50 ms a real head unit stopped recognising
            // presses, so detents vanish instead of tracking faster.
            uint16_t minPulseMs = 50;
            uint16_t minGapMs   = 30;
            // Stuck-key guard: a dropped Release must not leave a resistance on
            // the line forever.
            uint16_t maxHoldMs  = 12000;

            // ---- Spin::Hybrid ----
            // Pulsing N detents costs N*110 ms; holding costs 500 + N*50.
            // Equal at N≈8, below that holding is slower.
            uint8_t  holdAfter  = 8;
            uint16_t holdMaxMs  = 2500;
            // AOSP ViewConfiguration values (500 ms first repeat, 50 ms after);
            // unconfirmed for this head unit, whose MCU may decode the line.
            uint16_t repeatStartMs = 500;
            uint16_t repeatMs      = 50;
        };

        void loadDefaults();

        const Slot& slot(Keys::Action a) const;
        void setSlot(Keys::Action a, Slot s);

        ResistiveLadder&       ladder(uint8_t ch)       { return _lad[ch < CHANNELS ? ch : 0]; }
        const ResistiveLadder& ladder(uint8_t ch) const { return _lad[ch < CHANNELS ? ch : 0]; }

        Timing&       timing()       { return _t; }
        const Timing& timing() const { return _t; }

        Spin spin() const { return _spin; }
        void setSpin(Spin s) { _spin = s; }

        // true = GPIO drives a MOSFET gate (HIGH engages); false = GPIO is the
        // switch, LOW to engage, Hi-Z otherwise. Use MOSFETs on a unit whose
        // KEY line idles above 3.3 V (a Hi-Z pin's clamp would conduct).
        bool activeHigh() const { return _activeHigh; }
        void setActiveHigh(bool v) { _activeHigh = v; }

        uint8_t usedMasks(uint8_t ch, uint8_t* out, Keys::Action* who = nullptr) const;

        // Give each listed action its own rung, spread as widely as the ladder
        // allows. `actions` is in priority order (matters when rungs run out).
        uint8_t autoAssign(uint8_t ch, const Keys::Action* actions, uint8_t n);

        static constexpr uint16_t BLOB_SIZE =
            1                                   // version
          + 1                                   // flags
          + 1                                   // spin
          + CHANNELS                            // leg counts
          + CHANNELS * ResistiveLadder::MAX_LEGS * 2
          + CHANNELS * 2                        // series ohms
          + 18                                  // Timing
          + ACTIONS                             // packed slots (ch + mask)
          + ACTIONS;                            // long-press rungs
        void toBlob(uint8_t* out) const;
        bool fromBlob(const uint8_t* in, uint16_t len);

    private:
        ResistiveLadder _lad[CHANNELS];
        Slot            _slot[ACTIONS];
        Timing          _t;
        Spin            _spin = Spin::Hybrid;       // as loadDefaults()
        bool            _activeHigh = false;        // as loadDefaults()
    };
}
