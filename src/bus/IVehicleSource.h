#pragma once
#include <stddef.h>
#include <stdint.h>
#include "Frame.h"

// Somewhere the car's frames come from (docs/design/event-tasks.md, Step 5).
//
// SOURCES ARE CHOSEN AT BOOT, NOT AT BUILD TIME. The car task holds a list,
// built in setup() from the board and the build:
//   Mcp2518Source      T-2CAN, CAN A (FEATURE_CAN2): the chip, on the car task
//   TwaiVehicleSource  env:direct (no panel): frames the TWAI owner hands over
//
// Every method runs on the car task, which owns whatever is behind the source
// (an SPI chip, a UART), so none of it needs a lock. Nothing here blocks.
struct IVehicleSource {
    struct Stats {
        uint32_t frames  = 0;   // handed to the car task since boot
        uint32_t dropped = 0;   // lost before that: overflow, decode errors
    };

    virtual ~IVehicleSource() = default;

    virtual const char* name() const = 0;

    // Once, on the car task, before the first drain(). False = it did not
    // start; the source logs why, and may retry from service().
    virtual bool begin() = 0;

    // Every wake, before drain(): whatever is not a frame (a rate search, a
    // link greeting, a retry).
    virtual void service(uint32_t nowMs) { (void)nowMs; }

    // Up to `max` frames into `out`; how many. `max` back means there may be
    // more, and the car task comes straight back after a yield.
    virtual size_t drain(Frame* out, size_t max, uint32_t nowMs) = 0;

    // True: the source wakes the car task itself (an interrupt). False: the
    // car task polls it every 2 ms.
    virtual bool notifies() const { return false; }

    // The bus rate when known (the capture file header says it), else 0.
    virtual uint32_t bitrate() const { return 0; }

    virtual Stats stats() const = 0;
};
