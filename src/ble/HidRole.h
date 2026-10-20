#pragma once
// BLE HID consumer-control keyboard, as a role on a shared NimBLEServer.
// BleHub owns the stack, advertising and server callbacks.

#include <NimBLEDevice.h>
#include <stdint.h>

// USB HID Consumer Control usage codes
#define KEY_MEDIA_NEXT_TRACK      0x00B5
#define KEY_MEDIA_PREVIOUS_TRACK  0x00B6
#define KEY_MEDIA_PLAY_PAUSE      0x00CD
#define KEY_MEDIA_VOLUME_UP       0x00E9
#define KEY_MEDIA_VOLUME_DOWN     0x00EA

namespace Hid
{
    // Build the HID service tree; BleHub starts the server and advertises.
    void begin(NimBLEServer *server);

    // Drains the deferred key release. Loop task only.
    void service();

    // Queue a press; the release goes out on the next service(). Back-to-back
    // press/release can share a connection event and some head units drop it.
    bool press(uint16_t usageCode);

    bool     connected();
    uint16_t connHandle();

    // Whether the HID tree was built at all (i.e. a HID-capable mode is active).
    bool built();
} // namespace Hid
