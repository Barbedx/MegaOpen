#pragma once
#include <stdint.h>
#include "../Features.h"

// The link over Bluetooth LE (docs/design/usb-link.md, "Over BLE"): one GATT
// service on BleHub's server carrying the same frame stream as the USB vendor
// interface. RX: the app writes (encrypted link only: the first write bonds);
// TX: the board notifies. The S3 has no classic Bluetooth, so no SPP.
namespace BlePipe
{
    constexpr const char* SERVICE_UUID = "4d4f0100-6c69-6e6b-8000-4d6567614f70";
    constexpr const char* RX_UUID      = "4d4f0101-6c69-6e6b-8000-4d6567614f70";  // app -> board, write
    constexpr const char* TX_UUID      = "4d4f0102-6c69-6e6b-8000-4d6567614f70";  // board -> app, notify
    constexpr uint32_t    RX_BUF = 8192;   // what the app may write ahead of the applink task

    struct PeerStats {
        bool     on = false;          // the service exists (BLE on, in any mode)
        bool     subscribed = false;  // a peer subscribed to TX on an encrypted link
        uint16_t mtu = 0;
        char     addr[18] = {0};
        uint32_t overflow = 0;        // bytes the RX buffer had no room for
        uint32_t unencrypted = 0;     // writes refused on a link not encrypted yet
        uint32_t notifyRetries = 0;   // notifications the host had no buffer for, retried
        uint32_t subscribes = 0;
    };
}

#if FEATURE_BLE_LINK
#include "LinkSession.h"
class NimBLEServer;
namespace BlePipe
{
    // BleHub::Begin, before the server starts: the service on `server`.
    void addService(NimBLEServer* server);
    // BleHub: a connection went away.
    void onDisconnect(uint16_t connHandle);
    // AppLink: the pipe, `wake` called on every write from the app. It
    // exists from boot; it is up only once BleHub started (bleMode not
    // off, on its own task, later than AppLink) and a peer subscribed.
    Link::Pipe& pipe(void (*wake)());
    void stats(PeerStats& out);
}
#endif
