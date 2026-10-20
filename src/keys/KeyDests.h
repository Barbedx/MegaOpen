#pragma once
#include <stdint.h>
#include "KeyAction.h"

// Where a key went: the routing destinations by the ids the catalog, the
// routing endpoint and the key report all use. Portable (native-tested).
namespace Keys
{
    enum Dest : uint8_t { DEST_UART, DEST_AMS, DEST_HID, DEST_RES, DEST_USB, DEST_COUNT };
    constexpr const char* const DEST_IDS[DEST_COUNT] = {"uart", "ams", "hid", "res", "usb"};

    // One destination as the keys task sees it at the press: its class mask,
    // whether it is in the chain and switched on (`live`, what /api/health
    // keys.dest reports), and whether it acts only on a Press (the BLE and USB sinks
    // ignore a Long, so an asLong row never reaches them).
    struct DestState {
        uint8_t mask      = 0;
        bool    live      = false;
        bool    pressOnly = false;
    };

    // The classes the USB keyboard has usages for; phone and ui keys it drops.
    constexpr uint8_t USB_CLASSES = CLASS_TRANSPORT | CLASS_VOLUME;

    // Bit i set: destination i takes the key. `cls` is classOf(action); `isLong`
    // is whether the key's first edge is a Long (an asLong row).
    inline uint8_t keyDests(uint8_t cls, bool isLong, const DestState (&d)[DEST_COUNT])
    {
        if (cls == CLASS_NONE) return 0;
        uint8_t out = 0;
        for (uint8_t i = 0; i < DEST_COUNT; ++i) {
            if (i == DEST_USB && !(cls & USB_CLASSES)) continue;
            if (d[i].live && (d[i].mask & cls) && !(isLong && d[i].pressOnly))
                out |= (uint8_t)(1u << i);
        }
        return out;
    }

    // The last panel key, as the keys task resolved it: published through a
    // Snapshot<PanelKeyLast> for the wiring check (/api/affamap?live=1).
    struct PanelKeyLast {
        uint16_t wire   = 0;       // the raw wire code
        bool     hold   = false;
        uint8_t  key    = 0xFF;    // AffaMap key index; 0xFF = no row for this code
        uint8_t  action = 0;       // Keys::Action; None when unmapped
        uint8_t  dests  = 0;       // keyDests() bits
    };
}
