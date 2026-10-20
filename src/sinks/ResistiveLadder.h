#pragma once
#include <stdint.h>

// The analog half of KEY1/KEY2, as arithmetic (ARCHITECTURE.md §6.2).
//
// The head unit's steering-wheel input is an ADC with its own pull-up; a button
// is a resistor to ground, learned on the unit's key-study page by its voltage.
//
// N legs, each one resistor to ground behind one switch, sharing the KEY line:
//
//      KEY1 ──┬──────┬──────┬──────┐
//             R0     R1     R2     R3
//             │      │      │      │
//            sw0    sw1    sw2    sw3          (MOSFET, gate = one GPIO)
//             │      │      │      │
//      GND ───┴──────┴──────┴──────┘
//
// Engaged legs are in parallel, so N legs give 2^N-1 levels.
//
// Portable integer math (no Arduino, no float) so host tests assert exact values.
namespace Sinks
{
    class ResistiveLadder {
    public:
        // ResistiveMap packs a mask into five bits.
        static constexpr uint8_t MAX_LEGS = 5;

        // All switches open. A sentinel, not a large resistance: never compare
        // it against a threshold.
        static constexpr uint16_t OPEN = 0xFFFF;

        // Sanity rails on stored values; deliberately wide.
        static constexpr uint16_t MIN_OHMS = 50;
        static constexpr uint16_t MAX_OHMS = 60000;

        // What the head unit resolves (from forum/blog reports, not a datasheet):
        // above ~5 kΩ reads as open; the floor is thermal drift. Advisory only,
        // shown as a warning on the settings page.
        static constexpr uint16_t USABLE_MIN_OHMS = 100;
        static constexpr uint16_t USABLE_MAX_OHMS = 5000;

        // True when every populated leg is inside that window.
        bool inUsableWindow() const;

        void setLegCount(uint8_t n) { _n = n > MAX_LEGS ? MAX_LEGS : n; }
        uint8_t legCount() const { return _n; }

        void setLeg(uint8_t i, uint16_t ohms);
        uint16_t leg(uint8_t i) const { return i < MAX_LEGS ? _leg[i] : 0; }

        // Switch on-resistance plus harness, added to every level: 2N7002 ~2 Ω,
        // an ESP32 pin driving low 25-40 Ω.
        void setSeriesOhms(uint16_t r) { _series = r; }
        uint16_t seriesOhms() const { return _series; }

        // Legs engaged, one bit each. Mask 0 is OPEN.
        uint16_t ohmsFor(uint8_t mask) const;

        // Every mask this ladder can produce: 1 .. 2^n-1.
        uint8_t levelCount() const { return _n ? (uint8_t)((1u << _n) - 1) : 0; }

        // ---- choosing which levels to use ----
        // Parallel combinations cluster, so pick `count` masks spread as widely
        // as possible, lowest resistance first. Returns how many were placed,
        // possibly fewer than asked.
        uint8_t chooseMasks(uint8_t count, uint8_t* out) const;

        // Tightest gap between any two masks, % of the smaller. 0 = two masks
        // on the same rung; 255 = at least 255%.
        uint8_t worstSeparationPct(const uint8_t* masks, uint8_t n) const;

    private:
        // Conductance in nS: five legs of 50 Ω sum to 1e8, fits a uint32.
        static uint32_t nS(uint16_t ohms) { return ohms ? (uint32_t)(1000000000UL / ohms) : 0; }

        uint8_t  _n = 0;
        uint16_t _leg[MAX_LEGS] = {};
        uint16_t _series = 0;
    };
}
