#pragma once
#include <stdint.h>
#include "../Features.h"

// The USB link to the head unit's app (docs/design/usb-link.md): a vendor
// interface on the S3's own USB, frames from src/link/LinkFrame.h, HTTP piped
// to the board's own web server on 127.0.0.1, the panel alerts pushed as
// events. One task, "usblink", owns the interface and every socket it opens.
namespace UsbLink
{
    // Two requests at a time: each holds two of the web server's four sockets.
    constexpr uint8_t  CHANS = 2;
    constexpr uint32_t IDLE_MS = 15000;    // a channel this quiet is closed
    constexpr uint32_t ALIVE_MS = 3000;    // a frame this recent: the app is there

    struct Stats {
        bool     mounted = false;     // a host configured the device
        bool     alive = false;       // the app sent a frame in the last ALIVE_MS
        bool     hello = false;       // ... and said HELLO since it was plugged
        uint8_t  open = 0;            // channels with a socket
        uint32_t rxFrames = 0, txFrames = 0, bad = 0;
        uint32_t reqs = 0, fails = 0, events = 0;
        uint32_t rxBytes = 0, txBytes = 0;
        uint32_t lastRxAgoMs = 0;     // UINT32_MAX: never
    };

#if FEATURE_USB_LINK
    // After the web server listens. `name` is the board's AP name, for HELLO.
    void begin(const char* name);
    // Any task. The WiFi policy asks this.
    bool alive();
    void stats(Stats& out);
#else
    inline void begin(const char*) {}
    inline bool alive() { return false; }
    inline void stats(Stats& out) { out = Stats{}; }
#endif
}
