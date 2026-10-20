#include "KnobSource.h"

namespace Knobs
{
    void Source::configure(const Config& c)
    {
        _c = c;
        _c.sanitize();

        for (uint8_t k = 0; k < ENCODERS; ++k) {
            _quad[k].setPulsesPerDetent(_c.enc[k].ppd);
            _quad[k].setInverted(_c.enc[k].inverted);
        }
        // Re-seed from the next sample.
        reseed();

        Input::PotStepper::Params p;
        p.rawMin = _c.potMin; p.rawMax = _c.potMax;
        p.zones = _c.potZones; p.inverted = _c.potInverted;
        // Re-seeds so new zones don't read as a jump in volume.
        _pot.configure(p);
    }

    void Source::reseed()
    {
        for (uint8_t k = 0; k < ENCODERS; ++k) {
            _quad[k].reset();
            _btn[k].seeded = false;
        }
    }

    void Source::fire(const Binding& b, uint32_t nowMs)
    {
        if (!_emit || b.action == Keys::Action::None) return;
        Keys::Event e;
        e.action   = b.action;
        e.state    = Keys::State::Press;
        e.tMs      = nowMs;
        e.srcId    = SRC_ID;
        e.codeHint = b.code;
        _emit(e, !Keys::isEncoder(b.action), _ctx);
    }

    int8_t Source::sampleEncoder(uint8_t k, bool a, bool b, uint32_t nowMs)
    {
        if (k >= ENCODERS || !_c.enc[k].enabled) return 0;
        const int8_t d = _quad[k].update(a, b, nowMs);
        if (d > 0)      fire(_c.enc[k].cw, nowMs);
        else if (d < 0) fire(_c.enc[k].ccw, nowMs);
        return d;
    }

    bool Source::sampleButton(uint8_t k, bool down, uint32_t nowMs)
    {
        if (k >= ENCODERS || !_c.enc[k].enabled) return false;
        const Enc& e = _c.enc[k];
        Button& s = _btn[k];

        // A button held through a reset is not a press.
        if (!s.seeded) {
            s.seeded = true;
            s.stable = s.cand = down;
            s.sinceMs = nowMs;
            s.holdFired = true;
            return false;
        }

        if (down != s.cand) { s.cand = down; s.sinceMs = nowMs; }

        bool pressed = false;
        if (s.cand != s.stable && (uint32_t)(nowMs - s.sinceMs) >= DEBOUNCE_MS) {
            s.stable = s.cand;
            if (s.stable) {
                pressed = true;
                s.downMs = nowMs;
                s.holdFired = false;
                // No hold bound: click on the way down. Otherwise wait for release.
                if (e.hold.action == Keys::Action::None) {
                    s.holdFired = true;
                    ++s.clicks;
                    fire(e.click, nowMs);
                }
            } else if (!s.holdFired) {
                ++s.clicks;
                fire(e.click, nowMs);
            }
        }

        if (s.stable && !s.holdFired && (uint32_t)(nowMs - s.downMs) >= e.holdMs) {
            s.holdFired = true;
            ++s.holds;
            fire(e.hold, nowMs);
        }
        return pressed;
    }

    int8_t Source::samplePot(uint16_t raw, uint32_t nowMs)
    {
        // Fed even when Off: calibration needs a live reading.
        const int8_t steps = _pot.update(raw);
        if (!steps || _c.potMode != PotMode::Steps) return steps;

        int8_t n = steps > 0 ? steps : (int8_t)-steps;
        if (n > MAX_POT_BURST) n = MAX_POT_BURST;
        const Binding& b = steps > 0 ? _c.potUp : _c.potDown;
        for (int8_t i = 0; i < n; ++i) { ++_potSteps; fire(b, nowMs); }
        return steps;
    }
}
