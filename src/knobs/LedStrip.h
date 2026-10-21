#pragma once
#include "../Features.h"
#if FEATURE_KNOBS
#include <stdint.h>
#include "KnobConfig.h"

// WS2812B (or SK6812 RGBW: Order::GRBW) on one RMT channel. Not Adafruit_NeoPixel: on this core it installs
// the RMT driver in every show() (malloc/free per frame) and blocks. This
// installs once, allocates nothing afterwards and returns while the frame is
// on the wire. RMT timing is immune to WiFi/BLE interrupts.
namespace Knobs
{
    class LedStrip {
    public:
        bool begin(int8_t pin);
        bool ok() const { return _ok; }

        // `rgb` is count R,G,B triplets; `order` is the chips' wire order
        // (packWire(): three bytes a LED, four for RGBW).
        // Non-blocking; returns false (frame dropped) if the previous is still out.
        bool show(const uint8_t* rgb, uint8_t count, Order order);

        uint32_t frames() const { return _frames; }
        // Frames dropped as busy. Should be zero: 160 LEDs = 5 ms, frames 20 ms apart.
        uint32_t busy() const { return _busy; }

    private:
        bool     _ok = false;
        uint32_t _frames = 0;
        uint32_t _busy = 0;
    };
}
#endif
