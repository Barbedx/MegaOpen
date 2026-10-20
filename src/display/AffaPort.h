#pragma once
#include <AffaDisplay.h>
#include <Arduino.h>

// The firmware's side of the OEM display: clock, log sink, panel factory and
// Result names. The protocol itself is AffaDisplay's.
namespace MegaOpen
{
    // No delayMs(): the library must never sleep.
    struct ArduinoClock final : affa::IClock {
        uint32_t millis() const override { return ::millis(); }
    };

    // A sink is a leaf: it can be called mid-transition and must never call back
    // into the library, so it only formats and forwards.
    class AffaLogSink final : public affa::ILogSink {
    public:
        using EmitFn = void (*)(const char* line);
        void begin(EmitFn e) { _emit = e; }
        void write(uint8_t level, const char* tag, const char* msg) override;

    private:
        EmitFn _emit = nullptr;
    };

    // `type` is the string the web UI and NVS store. `carminat` is set only when
    // the panel built is a CarminatDisplay (for setTextStyled/showMessageBox/
    // selectBoxButton/showNavBitmap); an out-param because -fno-rtti rules out
    // dynamic_cast at the call site.
    affa::AffaDisplayBase* createDisplay(const char* type,
                                         affa::ICanLink& link, affa::IClock& clock,
                                         affa::CarminatDisplay** carminat = nullptr);

    // Human-readable affa::Result (0..5 keep the legacy AffaError values).
    const char* resultName(affa::Result r);
}
