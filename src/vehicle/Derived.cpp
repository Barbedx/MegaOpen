#include "Derived.h"
#include <stddef.h>

namespace Vehicle
{
    namespace
    {
        const uint8_t kGear[]  = {SIG_SPEED, SIG_RPM, SIG_NEUTRAL, SIG_CLUTCH, SIG_REVERSE, SIG_REVERSE_UPC};
        const uint8_t kRatio[] = {SIG_SPEED, SIG_RPM};
        const uint8_t kRate[]  = {SIG_FUEL_COUNTER};
        const uint8_t kCons[]  = {SIG_FUEL_COUNTER, SIG_SPEED};
        const uint8_t kRange[] = {SIG_FUEL_COUNTER, SIG_SPEED, SIG_FUEL_LEVEL};
        const uint8_t kNoLights[]  = {SIG_SPEED, SIG_LIGHT_DIPPED, SIG_LIGHT_MAIN};
        const uint8_t kHandbrake[] = {SIG_SPEED, SIG_HANDBRAKE};

        template <size_t N>
        uint8_t give(const uint8_t (&a)[N], const uint8_t*& list) { list = a; return (uint8_t)N; }

        void store(State& s, uint8_t sig, int64_t v, uint32_t now)
        {
            if (v > INT32_MAX) v = INT32_MAX;
            s.set(sig, State::clamp(sig, (int32_t)v), now);
        }
    }

    uint8_t Derived::inputs(uint8_t sig, const uint8_t*& list)
    {
        switch (sig) {
            case SIG_GEAR:       return give(kGear, list);
            case SIG_GEAR_RATIO: return give(kRatio, list);
            case SIG_FUEL_RATE:
            case SIG_TRIP_FUEL:  return give(kRate, list);
            case SIG_CONS_INST:
            case SIG_CONS_AVG:
            case SIG_TRIP_KM:
            case SIG_AVG_SPEED:
            case SIG_TRIP_ELAPSED: return give(kCons, list);
            case SIG_RANGE_CALC: return give(kRange, list);
            case SIG_DRIVE_NO_LIGHTS: return give(kNoLights, list);
            case SIG_DRIVE_HANDBRAKE: return give(kHandbrake, list);
            default:             list = nullptr; return 0;
        }
    }

    void Derived::put(State& s, uint8_t sig, int64_t v, uint32_t now)
    {
        if (bound(sig)) return;   // a CAN row or a constant feeds it instead
        store(s, sig, v, now);
    }

    void Derived::setBound(uint8_t sig, bool on)
    {
        if (sig >= SIG_COUNT) return;
        if (on) _bound[sig >> 3] = (uint8_t)(_bound[sig >> 3] | (1u << (sig & 7)));
        else    _bound[sig >> 3] = (uint8_t)(_bound[sig >> 3] & ~(1u << (sig & 7)));
    }

    int8_t Derived::gearFor(const Gears& g, int32_t ratio)
    {
        int8_t best = 0;
        int32_t bestOff = 0;
        for (uint8_t i = 0; i < GEARS; ++i) {
            const int32_t r = g.ratio[i];
            if (!r) continue;
            const int32_t off = ratio > r ? ratio - r : r - ratio;
            if (off * 100 > r * BAND_PCT) continue;
            if (!best || off * g.ratio[best - 1] < bestOff * r) { best = (int8_t)(i + 1); bestOff = off; }
        }
        return best;
    }

    void Derived::update(State& s, uint32_t now)
    {
        if (s.stamp(SIG_SPEED) != _speedTs || s.stamp(SIG_RPM) != _rpmTs) {
            _speedTs = s.stamp(SIG_SPEED);
            _rpmTs   = s.stamp(SIG_RPM);
            gear(s, now);
            safety(s, now);
        }
        if (s.stamp(SIG_FUEL_COUNTER) != _cntTs) {
            _cntTs = s.stamp(SIG_FUEL_COUNTER);
            fuel(s, now);
        }
    }

    void Derived::gear(State& s, uint32_t now)
    {
        if (!s.has(SIG_SPEED) || !s.has(SIG_RPM)) return;
        const int32_t speed = s.get(SIG_SPEED), rpm = s.get(SIG_RPM);
        int32_t ratio = 0;
        if (rpm >= MIN_RPM) {
            ratio = (int32_t)((int64_t)speed * 100 / rpm);
            put(s, SIG_GEAR_RATIO, ratio, now);
        }
        int8_t g;
        if (s.get(SIG_REVERSE) || s.get(SIG_REVERSE_UPC))       g = -1;
        else if (s.get(SIG_NEUTRAL) || s.get(SIG_CLUTCH) ||
                 speed < CRAWL || rpm < MIN_RPM)                 g = 0;
        else                                                     g = gearFor(_g, ratio);
        put(s, SIG_GEAR, g, now);
    }

    void Derived::fuel(State& s, uint32_t now)
    {
        const uint8_t c = (uint8_t)s.get(SIG_FUEL_COUNTER);
        // The first frame, or one after a silence (key off, a bus gap): a new
        // base, no step.
        if (!_haveCnt || (uint32_t)(now - _lastMs) > GAP_MS) {
            _cnt = c;
            _lastMs = now;
            _haveCnt = true;
            return;
        }
        const uint32_t dt = now - _lastMs;
        const uint8_t step = (uint8_t)(c - _cnt);     // an 8-bit counter wraps
        _cnt = c;
        _lastMs = now;
        if (step > MAX_STEP) return;

        const int32_t speed = s.has(SIG_SPEED) ? s.get(SIG_SPEED) : 0;
        // km/h x100 over ms: v/100 km/h is v/360 mm a ms.
        const uint64_t mm = speed > 0 ? (uint64_t)speed * dt / 360 : 0;
        _winCounts += step; _winMs += dt; _winMm += mm;
        _totCounts += step; _totMm += mm; _totMs += dt;
        if (_winMs < WINDOW_MS) return;

        // 80 mm³ a count: L/h = counts x 288 / ms; L/100km = counts x 8 / mm x 1000.
        put(s, SIG_FUEL_RATE, (int64_t)_winCounts * 2880 / _winMs, now);
        if (speed >= 500 && _winMm) put(s, SIG_CONS_INST, (int64_t)_winCounts * 80000 / (int64_t)_winMm, now);
        _winCounts = 0; _winMs = 0; _winMm = 0;

        put(s, SIG_TRIP_FUEL, (int64_t)(_totCounts * 8 / 1000), now);
        const int64_t km10 = (int64_t)(_totMm / 100000);
        put(s, SIG_TRIP_KM, km10, now);
        // The trip computer's pair, as the cluster keeps them: time with the
        // engine on, and distance over that time (from 1 km, when it means
        // something). km/h x100 = mm / ms x 360.
        put(s, SIG_TRIP_ELAPSED, (int64_t)(_totMs / 60000), now);
        if (_totMm >= AVG_SPEED_MIN_MM && _totMs) put(s, SIG_AVG_SPEED, (int64_t)(_totMm * 360 / _totMs), now);
        if (km10 < AVG_MIN_KM || !_totMm) return;
        const int64_t avg = (int64_t)(_totCounts * 80000 / _totMm);
        put(s, SIG_CONS_AVG, avg, now);
        if (avg > 0 && s.has(SIG_FUEL_LEVEL)) put(s, SIG_RANGE_CALC, (int64_t)s.get(SIG_FUEL_LEVEL) * 1000 / avg, now);
    }
}

namespace Vehicle
{
    // Moving with no headlights, or with the handbrake up: each a yes/no with
    // its own speed band (on at or above, off below the lower edge), so a car
    // crawling round the threshold does not flicker. No speed, no light or no
    // handbrake value: no.
    void Derived::safety(State& s, uint32_t now)
    {
        const int32_t v = s.has(SIG_SPEED) ? s.get(SIG_SPEED) : 0;   // km/h x100
        _movingLights = _lightsOn != INT32_MAX && v >= (_movingLights ? _lightsOn / 2 : _lightsOn);
        _movingBrake  = _brakeOn != INT32_MAX && v >= (_movingBrake ? _brakeOn / 2 : _brakeOn);
        const bool beams = (s.has(SIG_LIGHT_DIPPED) && s.get(SIG_LIGHT_DIPPED)) ||
                           (s.has(SIG_LIGHT_MAIN) && s.get(SIG_LIGHT_MAIN));
        const bool knownLights = s.has(SIG_LIGHT_DIPPED) || s.has(SIG_LIGHT_MAIN);
        put(s, SIG_DRIVE_NO_LIGHTS, _movingLights && knownLights && !beams ? 1 : 0, now);
        put(s, SIG_DRIVE_HANDBRAKE,
            _movingBrake && s.has(SIG_HANDBRAKE) && s.get(SIG_HANDBRAKE) ? 1 : 0, now);
    }
}
