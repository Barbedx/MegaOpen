#pragma once
#include <Arduino.h>
#include "../wire/WireLink.h"

// WireProto — the contract with PC-side tools (serial proxy / phone recorder).
// Formats tagged lines and fans them out to every registered WireLink. Keep the
// tags in sync with the proxy parser.
//
// One message per line; tagged lines start with '@', anything else is free text.
//
//   firmware -> PC
//     @TX <id-hex> <b0> <b1> ...    outbound CAN frame (display <- ESP). The feed the
//                                   PC virtual display decodes (AFFA3 ISO-TP).
//     @RX <id-hex> <b0> ...         inbound CAN frame (ESP <- bus) — live car capture.
//     @EV <name> <k=v> ...          structured event. (future)
//
//   PC -> firmware  (delivered via WireProto::dispatchCommand from a link)
//     @KEY <code-dec> <hold>        emulate a display key press.
//     @INJ <id-hex> <b0> ...        inject a CAN RX frame (ACK the display handshake).
//     @EMU <0|1>                    toggle bench emulator self-ACK.
//
// The @TX byte format is frozen (serial_proxy.py parses it).

namespace WireProto
{
    constexpr const char *TAG_TX  = "@TX";
    constexpr const char *TAG_RX  = "@RX";
    constexpr const char *TAG_EV  = "@EV";
    constexpr const char *TAG_KEY = "@KEY";
    constexpr const char *TAG_INJ = "@INJ";

    // Register an outbound link (main.cpp's BusWireLink: the LogBus). Up to MAX_LINKS.
    void addLink(WireLink *link);

    // Register the single inbound-command handler; links call dispatchCommand().
    void onCommand(WireLink::CommandCb cb, void *ctx);
    void dispatchCommand(const char *line);

    // Format "<tag> <id> <b0> <b1> ..." into buf; returns the written length.
    int buildFrameLine(char *buf, int size, const char *tag,
                       uint32_t id, const uint8_t *data, uint8_t len);

    // Emit one outbound CAN frame as "@TX <id> <bytes>" to all links.
    void emitTx(uint32_t id, const uint8_t *data, uint8_t len);

    // Broadcast an already-formatted line to all links.
    void emitLine(const char *line);
}
