#include "OtaVerdict.h"

OtaVerdict::Act OtaVerdict::tick(uint32_t nowMs, bool healthy)
{
    if (!_trial || _done) return Act::None;
    // Healthy for CONFIRM_MS without a break, not one good sample after it.
    // A flag, not a 0 sentinel: with one, a sample at t=0 confirmed at once.
    if (!healthy)        _healthy = false;
    else if (!_healthy) { _healthy = true; _healthySince = nowMs; }
    if (_healthy && (uint32_t)(nowMs - _healthySince) >= CONFIRM_MS) {
        _done = true;
        return Act::Confirm;
    }
    if (nowMs > TRIAL_MS) {
        _done = true;
        return Act::Revert;
    }
    return Act::None;
}
