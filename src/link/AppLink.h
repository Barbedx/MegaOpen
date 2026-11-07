#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../Features.h"
#include "LinkSession.h"
#include "UsbPipe.h"
#include "BlePipe.h"

// The board's link to MegaOpen Companion (docs/design/usb-link.md): the same
// frames over USB (UsbPipe) and over Bluetooth LE (BlePipe), one session per
// transport, both served by one task, "applink". This is what the rest of the
// firmware asks: is an app there, and may it carry the board's internet.
//
// Policy (owner, 2026-11-07): USB is the way in when it works, and then the
// board's WiFi goes off (WifiManager asks usbNet). BLE never turns WiFi
// off: it carries the page and the alerts, and the board's internet only
// when the board's WiFi has none (the head unit's hotspot gone to 5 GHz).
namespace AppLink
{
    enum class Via : uint8_t { None, Usb, Ble };
    const char* viaName(Via v);   // "none", "usb", "ble"

    struct Stats {
        Via                via = Via::None;     // what the app uses now (USB first)
        Link::SessionStats usb, ble;
        UsbPipe::BusStats  bus;
        BlePipe::PeerStats peer;
    };

#if FEATURE_APP_LINK
    // After the web server listens. `name` is the board's AP name, for HELLO.
    void begin(const char* name);
    // main: is the board's own WiFi online (station, connected)? The net
    // calls ask it to choose between WiFi and BLE.
    void setWifiOnline(bool (*online)());

    bool alive();          // an app on any transport
    bool usbNet();         // an app on USB that offered its internet: the WiFi policy's question
    Via  via();
    void stats(Stats& out);

    // ---- the app's internet ------------------------------------------------
    // The board's own GitHub requests (GhOta, LogPush) carried out by the app
    // with Android's TLS: the board composes them as over WiFi, the app only
    // moves the bytes. One at a time (NetJob serialises the callers); any
    // task but applink.
    //
    // True when the next request should go through the app rather than
    // WiFi: an app on USB that offered it, or one on BLE while WiFi is offline.
    bool netFirst();
    // Any app that offered its internet (LogPush's "is there a way out").
    bool netAvailable();
    // Sends the request (headers: name, value, ..., nullptr) and waits for the
    // answer's head. False with `why` (a short code) when there is none.
    bool netOpen(const char* method, const char* url, const char* const* headers,
                 const uint8_t* body, size_t len, uint32_t timeoutMs, Link::NetHead& head, const char*& why);
    // Body bytes: >0, 0 at the end, -1 on a failure or timeout.
    int  netRead(uint8_t* buf, size_t n, uint32_t timeoutMs);
    void netClose();
#else
    inline void begin(const char*) {}
    inline void setWifiOnline(bool (*)()) {}
    inline bool alive() { return false; }
    inline bool usbNet() { return false; }
    inline Via  via() { return Via::None; }
    inline void stats(Stats& out) { out = Stats{}; }
    inline bool netFirst() { return false; }
    inline bool netAvailable() { return false; }
    inline bool netOpen(const char*, const char*, const char* const*, const uint8_t*, size_t, uint32_t,
                        Link::NetHead&, const char*& why) { why = "no_app"; return false; }
    inline int  netRead(uint8_t*, size_t, uint32_t) { return -1; }
    inline void netClose() {}
#endif
}
