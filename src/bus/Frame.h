#pragma once
#include <stdint.h>

// Portable CAN frame, no driver dependency, so code above the controller builds
// on the native host. TwaiBus (env:direct) and the CAN A source convert to it;
// the panel's frames stay affa::Frame inside AffaDisplay.
struct Frame {
    // Which bus the frame came from, tagged at the conversion points.
    enum Source : uint8_t {
        SRC_MM_CAN  = 0,   // multimedia CAN (this board's TWAI controller)
        SRC_VH_CAN  = 1,   // vehicle main CAN, relayed by the VH board
    };

    uint32_t id = 0;
    uint8_t  len = 0;
    uint8_t  data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool     extended = false;
    uint8_t  source = SRC_MM_CAN;
};
