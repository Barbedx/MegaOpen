#pragma once
#include <stdint.h>
#include "../Features.h"

// Which bus the TWAI controller is on. It follows the build: the panel where
// there is one (FEATURE_DISPLAY_CAN), else the car (env:direct).
//
// In Vehicle role we must be completely silent: TX is gated off and the
// display registration handshake (which transmits AFFA frames) never runs.
namespace Bus
{
    enum class Role : uint8_t {
        Display = 0,   // multimedia bus: we are master, we register functions, we send
        Vehicle = 1,   // car bus: listen-only, never transmit
    };

    inline constexpr Role ROLE = FEATURE_DISPLAY_CAN ? Role::Display : Role::Vehicle;

    inline constexpr const char* roleName(Role r) { return r == Role::Vehicle ? "vehicle" : "display"; }
}
