#include "KeyDebouncer.h"

namespace Input
{
    void KeyDebouncer::reset()
    {
        _stable = 0;
        for (uint8_t i = 0; i < POSITIONS; ++i) _pos[i] = Pos{};
    }

    uint8_t KeyDebouncer::update(uint16_t raw, uint32_t nowMs, PosEvent* out, uint8_t maxOut)
    {
        uint8_t n = 0;
        auto emit = [&](uint8_t pos, Edge e) {
            if (out && n < maxOut) out[n++] = PosEvent{pos, e, nowMs};
        };

        for (uint8_t i = 0; i < POSITIONS; ++i) {
            Pos& p = _pos[i];
            const bool bit  = (raw >> i) & 1;
            const bool fast = (_fast >> i) & 1;
            const uint16_t settle = fast ? _p.fastDebounceMs : _p.debounceMs;

            if (bit != p.candidate) {          // raw moved: restart the settle timer
                p.candidate = bit;
                p.sinceMs   = nowMs;
            }

            if (p.candidate != p.stable && (uint32_t)(nowMs - p.sinceMs) >= settle) {
                p.stable = p.candidate;
                if (p.stable) {
                    p.downMs   = nowMs;
                    p.longSent = false;
                    p.nextRepeatMs = 0;
                    _stable |= (uint16_t)(1u << i);
                    emit(i, Edge::Down);
                } else {
                    _stable &= (uint16_t)~(1u << i);
                    emit(i, Edge::Up);
                }
            }

            // Hold behaviour is for real buttons only — an encoder phase that
            // stays closed means the wheel stopped between detents, not a hold.
            if (p.stable && !fast) {
                if (!p.longSent && (uint32_t)(nowMs - p.downMs) >= _p.longMs) {
                    p.longSent = true;
                    p.nextRepeatMs = nowMs + _p.repeatMs;
                    emit(i, Edge::Long);
                } else if (p.longSent && _p.repeatEnabled &&
                           (int32_t)(nowMs - p.nextRepeatMs) >= 0) {
                    p.nextRepeatMs = nowMs + _p.repeatMs;
                    emit(i, Edge::Repeat);
                }
            }
        }
        return n;
    }
}
