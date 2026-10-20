#pragma once
#include <stdint.h>

// Transport for WireProto lines (@TX/@RX/@EV out, @KEY/@INJ/@EMU in). WireProto
// fans outbound lines to every registered link (the LogBus, main.cpp's BusWireLink).
struct WireLink {
    using CommandCb = void (*)(const char* line, void* ctx);

    virtual ~WireLink() = default;

    // Dropped when no peer is connected.
    virtual void emitLine(const char* line) = 0;

    // Inbound line, NUL-terminated, e.g. "@INJ 3CF 61 11".
    virtual void onCommand(CommandCb cb, void* ctx) = 0;

    virtual bool connected() const = 0;
};
