#pragma once
#include <stddef.h>
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

    // ---- the app's internet ------------------------------------------------
    // The board's own GitHub requests (GhOta, LogPush), carried out by the
    // app on its network with Android's TLS: the board composes them as over
    // WiFi, the app only moves the bytes (docs/design/usb-link.md, "Net").
    // One at a time; TlsJob already serialises the callers. Any task but
    // usblink. The body is the app's to stream; 16 kB of credit at a time.
    constexpr size_t NET_WINDOW = 16384;
    struct NetHead {
        int     status = 0;
        int64_t len = -1;          // -1: the app does not know
        char    url[512] = {0};    // where it ended up, after the redirects
    };

#if FEATURE_USB_LINK
    // After the web server listens. `name` is the board's AP name, for HELLO.
    void begin(const char* name);
    // Any task. The WiFi policy asks this.
    bool alive();
    void stats(Stats& out);

    // An app is on the link and offered its internet.
    bool netAvailable();
    // Sends the request (headers: name, value, ..., nullptr) and waits for the
    // answer's head. False with `why` (a short code) when there is none.
    bool netOpen(const char* method, const char* url, const char* const* headers,
                 const uint8_t* body, size_t len, uint32_t timeoutMs, NetHead& head, const char*& why);
    // Body bytes: >0, 0 at the end, -1 on a failure or timeout.
    int  netRead(uint8_t* buf, size_t n, uint32_t timeoutMs);
    void netClose();
#else
    inline void begin(const char*) {}
    inline bool alive() { return false; }
    inline void stats(Stats& out) { out = Stats{}; }
    inline bool netAvailable() { return false; }
    inline bool netOpen(const char*, const char*, const char* const*, const uint8_t*, size_t, uint32_t,
                        NetHead&, const char*& why) { why = "no_link"; return false; }
    inline int  netRead(uint8_t*, size_t, uint32_t) { return -1; }
    inline void netClose() {}
#endif
}
