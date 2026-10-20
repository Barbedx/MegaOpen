#pragma once
#include <stdint.h>

// A potentiometer read as a KNOB: a filtered position, and the detents a knob
// would have had if it were an encoder. Portable; fed raw ADC counts.
//
// The travel is cut into zones; crossing a boundary is one step. The end stops
// resync the head unit's volume whenever the knob reaches an end.
//
//   NOISE   an ESP32 input wanders by tens of counts with the radio on: filtered,
//           and a boundary must be cleared by a margin (hysteresis).
//   NO EDGE the first reading seeds the zone and reports nothing.
namespace Input
{
    class PotStepper {
    public:
        struct Params {
            uint16_t rawMin   = 0;      // calibrated ends of travel, in ADC counts
            uint16_t rawMax   = 4095;
            uint8_t  zones    = 30;     // 2..64
            bool     inverted = false;
        };

        void configure(const Params& p);
        const Params& params() const { return _p; }

        // Feed one sample. Returns the zones crossed since the previous one:
        // positive is "up", negative "down", 0 on the seeding sample.
        int8_t update(uint16_t raw);

        void reset() { _seeded = false; }

        // Filtered ADC counts, never inverted (the calibration captures these).
        uint16_t raw()   const { return (uint16_t)(_filt >> 4); }
        // 0..255 along the travel, and the zone — both in the user's direction.
        uint8_t  level() const { return _p.inverted ? (uint8_t)(255 - _level) : _level; }
        uint8_t  zone()  const { return _p.inverted ? (uint8_t)(_p.zones - 1 - _zone) : _zone; }

    private:
        uint16_t span() const { return (uint16_t)(_p.rawMax - _p.rawMin); }
        uint8_t  zoneOf(int32_t v) const;

        Params   _p;
        bool     _seeded = false;
        uint32_t _filt = 0;         // 12.4 fixed point
        uint8_t  _zone = 0;
        uint8_t  _level = 0;
    };
}
