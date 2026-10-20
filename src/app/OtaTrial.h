#pragma once
#include <stdint.h>

// The OTA trial (APP_ROLLBACK): an OTA image boots PENDING_VERIFY and is
// marked valid once it has been healthy for OtaVerdict::CONFIRM_MS without a
// break; TRIAL_MS without that marks it invalid and reboots into the previous
// image. A trial cut short by a reset reverts too (the bootloader's rule).
// USB-flashed images are never PENDING_VERIFY and get no verdict.
//
// "Healthy" is net_svc's (Housekeeping.cpp): no crash, WiFi up at least once
// so the next OTA can arrive, heap above HEALTH_HEAP_MIN, and every critical
// task in the registry running. The display is deliberately not a condition:
// a bench without the panel must not roll good builds back.
namespace App
{
    namespace OtaTrial
    {
        constexpr uint32_t HEALTH_HEAP_MIN = 20000;   // bytes free for "healthy"

        // setup(): reads the running image's state and says whether it is on trial.
        void begin();
        bool onTrial();

        // net_svc, every pass.
        void tick(uint32_t nowMs, bool healthy);
    }
}
