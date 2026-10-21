#pragma once
#include <math.h>
#include <stdint.h>

// How many AMS volume commands one volume key press sends.
//
// Each AMS VolumeUp/VolumeDown is a write with response (30-130 ms of the BLE
// task), and how far one moves the phone's volume is not documented. The old
// fixed fifteen per press cost 450-750 ms of radio time a press. The phone
// reports its volume (AMS Player attribute Volume, 0..1), so the size of one
// command is measured instead: after a burst of same-direction commands has
// settled, the reported change divided by the writes is one command's step,
// and a press becomes as many commands as make one iPhone button step (1/16).
//
// It starts at one command a press, and stays there until a measurement says
// otherwise. A burst with no report (late, or a phone that does not send one)
// says nothing; two in a row before anything was learned mean this phone does
// not report, and only then does a press become FALLBACK_STEPS. A burst that
// ends at 0 or 1 was cut short and is not learned from.
//
// Pure (no Arduino): host-tested in test/test_amsvol.
namespace Sinks
{
    class AmsVolumeStep {
    public:
        static constexpr uint8_t MAX_STEPS   = 15;          // the old fixed count, now the cap
        static constexpr uint8_t FALLBACK_STEPS = 4;        // a phone that never reports
        static constexpr uint8_t SILENT_BURSTS  = 2;        // in a row, before that
        static constexpr float   PRESS       = 1.0f / 16;   // one iPhone volume button step
        static constexpr float   EDGE        = 0.001f;      // 0 or 1: clamped, not measured
        static constexpr float   NOISE       = 0.0001f;     // below this, nothing moved

        uint8_t steps() const { return _steps; }
        bool learned() const { return _learned; }
        // One command's step as last measured, 0 = not yet.
        float stepSize() const { return _step; }

        // A burst of `writes` same-direction commands moved the reported volume
        // from v0 to v1; `updated` = a volume notification arrived after the
        // burst began. True when it measured a step.
        bool observe(float v0, float v1, uint16_t writes, bool updated)
        {
            if (!writes) return false;
            if (atEdge(v0) || atEdge(v1)) return false;
            const float moved = fabsf(v1 - v0);
            if (!updated || moved < NOISE) {
                // Nothing reported. A learned count stays; an unlearned one
                // waits for a second silent burst before giving up on reports.
                if (!_learned && _silent < SILENT_BURSTS && ++_silent == SILENT_BURSTS)
                    _steps = FALLBACK_STEPS;
                return false;
            }
            _silent = 0;
            _step = moved / writes;
            long n = lroundf(PRESS / _step);
            if (n < 1) n = 1;
            if (n > MAX_STEPS) n = MAX_STEPS;
            _steps = (uint8_t)n;
            _learned = true;
            return true;
        }

    private:
        static bool atEdge(float v) { return v <= EDGE || v >= 1.0f - EDGE; }

        uint8_t _steps = 1;
        uint8_t _silent = 0;      // no-report bursts in a row, before a measurement
        bool    _learned = false;
        float   _step = 0;
    };
}
