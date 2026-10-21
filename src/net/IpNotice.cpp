#include "IpNotice.h"

bool IpNotice::tick(uint32_t nowMs, Mode mode, uint32_t ip)
{
    if (mode == Mode::None) {
        if (_up) { _up = false; _downAt = nowMs; _everDown = true; }
        return false;
    }
    if (_up) {
        // Up, but the lease or the mode moved under it without a down pass.
        if (mode == _shownMode && ip == _shownIp) return false;
    } else {
        _up = true;
        const bool same  = mode == _shownMode && ip == _shownIp;
        const bool flap  = _everDown && (uint32_t)(nowMs - _downAt) < FLAP_MS;
        if (same && flap) return false;
    }
    _shownMode = mode;
    _shownIp   = ip;
    return true;
}
