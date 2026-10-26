#pragma once
#include <stdint.h>
#include <stddef.h>
#include "VehicleState.h"

// Signals computed from decoded ones, on the car task after each frame's
// decode (it is State's only writer). Portable and native-tested (test_decode).
//
//   gear        a manual gearbox has no gear on CAN: the speed/rpm ratio is
//               matched against a table of km/h per 1000 rpm. R from either
//               reverse bit, N from the neutral bit, the clutch, or a crawl.
//   fuel        0x551's wrapping counter (80 mm³ a count) gives L/h; the
//               distance is speed integrated over time; litres over km give
//               L/100km, now (a window of >= WINDOW_MS) and since boot.
//   range       fuel_level over the average, once AVG_MIN_KM have been driven.
//
// Nothing here goes to the head unit: these are new signals, not range,
// fuel_inst or fuel_avg (the Raise ones).
namespace Vehicle
{
    class Derived {
    public:
        static constexpr uint8_t  GEARS      = 6;
        static constexpr uint32_t WINDOW_MS  = 1000;    // cons_inst's shortest window
        static constexpr uint32_t GAP_MS     = 2000;    // a counter silent longer: rebase
        static constexpr uint8_t  MAX_STEP   = 200;     // counts in one frame: a glitch
        static constexpr int32_t  CRAWL      = 300;     // km/h x100: below it, N
        static constexpr int32_t  MIN_RPM    = 400;
        static constexpr uint32_t AVG_MIN_KM = 50;      // km x10 = 5 km
        static constexpr uint64_t AVG_SPEED_MIN_MM = 1000000;   // 1 km before an average speed
        // km/h: where drive_no_lights and drive_handbrake start, by default
        // (Settings › Reminders). Each stops below half its speed, so a car
        // crawling round the threshold does not flicker.
        static constexpr uint8_t  LIGHTS_KMH = 10, BRAKE_KMH = 5;
        static constexpr uint8_t  BAND_PCT   = 12;      // a gear matches within ±12 %

        // km/h per 1000 rpm x10, 1st..6th; 0 = no such gear. The defaults are a
        // Mégane II 1.6 16V five-speed on 205/55 R16, computed, not measured:
        // the live gear_ratio on the page is what to copy here.
        struct Gears {
            uint16_t ratio[GEARS] = {75, 136, 200, 270, 352, 0};
        };

        void setGears(const Gears& g) { _g = g; }
        const Gears& gears() const { return _g; }

        // After a frame's decode: recomputes what that frame's inputs feed.
        void update(State& s, uint32_t nowMs);

        // The decoded signals a derived one is computed from; 0 when `sig` is
        // not derived. For Vehicle::verified(): a derived value is as good as
        // its weakest input.
        static uint8_t inputs(uint8_t sig, const uint8_t*& list);

        // The gear for a ratio (km/h per 1000 rpm x10), 0 when none is close.
        static int8_t gearFor(const Gears& g, int32_t ratio);

        // The reminders' speeds, km/h; 0 switches that reminder off.
        void setReminderSpeeds(uint8_t lightsKmh, uint8_t brakeKmh)
        {
            _lightsOn = lightsKmh ? (int32_t)lightsKmh * 100 : INT32_MAX;
            _brakeOn  = brakeKmh ? (int32_t)brakeKmh * 100 : INT32_MAX;
        }

    private:
        void gear(State& s, uint32_t now);
        void fuel(State& s, uint32_t now);
        void safety(State& s, uint32_t now);

        Gears    _g;
        int32_t  _lightsOn = LIGHTS_KMH * 100, _brakeOn = BRAKE_KMH * 100;   // km/h x100
        uint32_t _speedTs = 0, _rpmTs = 0, _cntTs = 0;
        uint8_t  _cnt = 0;
        bool     _haveCnt = false;
        // Distance: speed integrated between two counter frames, in mm.
        uint32_t _lastMs = 0;
        // This window, and since boot.
        uint32_t _winCounts = 0, _winMs = 0;
        uint64_t _winMm = 0;
        uint64_t _totCounts = 0, _totMm = 0;
        uint64_t _totMs = 0;       // engine-on time since boot (counter frames flowing)
        bool     _movingLights = false, _movingBrake = false;
    };
}
