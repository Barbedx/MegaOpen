#pragma once
#include <stdint.h>

// Which bus the single CAN controller is on. Same pins and 500 kbit/s either
// way, so it is a runtime setting: move the wires to OBD2 and flip it.
//
// In Vehicle role we must be completely silent: TX is gated off and the
// display registration handshake (which transmits AFFA frames) never runs.
namespace Bus
{
    enum class Role : uint8_t {
        Display = 0,   // multimedia bus: we are master, we register functions, we send
        Vehicle = 1,   // car bus: listen-only, never transmit
    };

    inline const char* roleName(Role r) { return r == Role::Vehicle ? "vehicle" : "display"; }
    inline Role roleFromName(const char* s) {
        return (s && s[0] == 'v') ? Role::Vehicle : Role::Display;
    }
}
