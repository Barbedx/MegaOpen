#pragma once
#include <stdint.h>

// Portable CAN frame, no driver dependency, so code above the controller builds
// on the native host. TwaiBus converts to/from twai_message_t; AffaPort.cpp
// converts to affa::Frame.
struct Frame {
    // Which bus the frame came from, tagged at the conversion points.
    enum Source : uint8_t {
        SRC_MM_CAN  = 0,   // multimedia CAN (this board's TWAI controller)
        SRC_VH_CAN  = 1,   // vehicle main CAN, relayed by the VH board
        SRC_VIRTUAL = 2,   // in-firmware virtual display (EmuBridge)
        SRC_INJECT  = 3,   // test/RE injection (@INJ / web)
    };

    uint32_t id = 0;
    uint8_t  len = 0;
    uint8_t  data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool     extended = false;
    uint8_t  source = SRC_MM_CAN;
};
