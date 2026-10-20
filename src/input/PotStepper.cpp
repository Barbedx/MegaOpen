#include "PotStepper.h"

namespace Input
{
    void PotStepper::configure(const Params& p)
    {
        _p = p;
        if (_p.zones < 2)  _p.zones = 2;
        if (_p.zones > 64) _p.zones = 64;
        if (_p.rawMax > 4095) _p.rawMax = 4095;
        // Bad calibration: fall back to the full range.
        if (_p.rawMax <= _p.rawMin || span() < 64) { _p.rawMin = 0; _p.rawMax = 4095; }
        _seeded = false;
    }

    uint8_t PotStepper::zoneOf(int32_t v) const
    {
        if (v <= (int32_t)_p.rawMin) return 0;
        if (v >= (int32_t)_p.rawMax) return (uint8_t)(_p.zones - 1);
        const uint32_t z = (uint32_t)(v - _p.rawMin) * _p.zones / ((uint32_t)span() + 1u);
        return (uint8_t)(z < _p.zones ? z : _p.zones - 1);
    }

    // Works in raw ADC direction; `inverted` applies only to the returned steps.
    int8_t PotStepper::update(uint16_t raw)
    {
        if (raw > 4095) raw = 4095;

        int8_t steps = 0;
        if (!_seeded) {
            _seeded = true;
            _filt = (uint32_t)raw << 4;
            _zone = zoneOf(raw);
        } else {
            // A quarter of the way per sample (~0.1 s to settle at 50 Hz). The last
            // fraction snaps, or the filter stalls short of 4095 at the stop.
            const int32_t want = (int32_t)raw << 4;
            const int32_t d = want - (int32_t)_filt;
            _filt = (uint32_t)((d > -4 && d < 4) ? want : (int32_t)_filt + d / 4);

            const int32_t v = (int32_t)(_filt >> 4);
            // A quarter of a zone, and never less than the noise floor.
            int32_t hyst = (int32_t)span() / ((int32_t)_p.zones * 4);
            if (hyst < 12) hyst = 12;

            const uint8_t zUp = zoneOf(v - hyst);
            const uint8_t zDn = zoneOf(v + hyst);
            uint8_t z = _zone;
            if (zUp > _zone)      z = zUp;
            else if (zDn < _zone) z = zDn;

            steps = (int8_t)((int)z - (int)_zone);
            _zone = z;
        }

        int32_t lv = ((int32_t)(_filt >> 4) - (int32_t)_p.rawMin) * 255 / (int32_t)span();
        if (lv < 0)   lv = 0;
        if (lv > 255) lv = 255;
        // Two counts of slack against shimmer; the ends are exempt so 0/255 are reachable.
        const int32_t dl = lv - (int32_t)_level;
        if (dl >= 2 || dl <= -2 || lv == 0 || lv == 255) _level = (uint8_t)lv;

        return (int8_t)(_p.inverted ? -steps : steps);
    }
}
