#pragma once
#include <AffaDisplay.h>
#include <Arduino.h>
#include "AffaLogSink.h"

// The firmware's side of the OEM display: clock and panel factory (the log
// sink is AffaLogSink.h). The protocol itself is AffaDisplay's, and so are the
// Result names (affa::resultName).
namespace MegaOpen
{
    // No delayMs(): the library must never sleep.
    struct ArduinoClock final : affa::IClock {
        uint32_t millis() const override { return ::millis(); }
    };

    // `type` is the string the web UI and NVS store. `carminat` is set only when
    // the panel built is a CarminatDisplay (for setTextStyled/showMessageBox/
    // selectBoxButton/showNavBitmap); an out-param because -fno-rtti rules out
    // dynamic_cast at the call site.
    affa::AffaDisplayBase* createDisplay(const char* type,
                                         affa::ICanLink& link, affa::IClock& clock,
                                         affa::CarminatDisplay** carminat = nullptr);
}
