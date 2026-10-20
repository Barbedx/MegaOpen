#include "Autobaud.h"

namespace Bus
{
    namespace
    {
        // The owner's order: the common car rates first, 1M last.
        constexpr uint32_t kRates[Autobaud::N_RATES] = {500000, 250000, 125000, 1000000};
    }

    uint32_t Autobaud::rateAt(uint8_t i) { return i < N_RATES ? kRates[i] : 0; }

    bool Autobaud::valid(uint32_t bps)
    {
        for (uint32_t r : kRates)
            if (r == bps) return true;
        return false;
    }

    void Autobaud::begin(uint32_t manualBps, uint32_t savedBps, uint32_t nowMs, uint32_t dwellMs)
    {
        _n = 0;
        _i = 0;
        _cycles = 0;
        _rejected = 0;
        _dwellMs = dwellMs ? dwellMs : DEFAULT_DWELL_MS;
        _startedMs = nowMs;
        if (valid(manualBps)) {
            _order[_n++] = manualBps;
            _state = State::Manual;
            return;
        }
        _state = State::Searching;
        if (valid(savedBps)) _order[_n++] = savedBps;
        for (uint32_t r : kRates)
            if (r != savedBps) _order[_n++] = r;
    }

    void Autobaud::next()
    {
        _i = (uint8_t)(_i + 1);
        if (_i >= _n) {
            _i = 0;
            ++_cycles;
        }
    }

    Autobaud::Step Autobaud::observe(uint32_t nowMs, uint32_t frames, uint32_t errors)
    {
        if (_state != State::Searching) return Step::Stay;
        if (errors) {
            ++_rejected;
            next();
            return Step::Next;
        }
        if (frames >= LOCK_FRAMES) {
            _state = State::Locked;
            return Step::Lock;
        }
        if ((uint32_t)(nowMs - _startedMs) < _dwellMs) return Step::Stay;
        if (frames) {
            _state = State::Locked;
            return Step::Lock;
        }
        next();
        return Step::Next;
    }
}
