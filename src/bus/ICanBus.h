#pragma once
#include "Frame.h"

// Plain function pointer, not std::function: no heap/RTTI on the ESP32.
using RxHandler = void (*)(const Frame&, void* ctx);

// Push-style CAN port; TwaiBus is the only implementation. AffaDisplay needs a
// pull port (recv(Frame&)) so it owns its drain; src/display/AffaPort.* adapts.
struct ICanBus {
    virtual ~ICanBus() = default;
    virtual bool send(const Frame& f) = 0;          // false if dropped (e.g. !isLive)
    virtual void onReceive(RxHandler h, void* ctx) = 0;
    virtual bool isLive() const = 0;
    virtual void poll() {}                          // drain the driver's RX queue
};
