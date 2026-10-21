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

    // The classes each sink has commands for: AMS, HID and the USB keyboard
    // have transport and volume, no phone or ui keys. A mask bit outside
    // them is never routed, so the routing cannot promise what a sink drops.
    constexpr uint8_t DEST_CLASSES[DEST_COUNT] = {
        CLASS_ALL,                          // uart
        CLASS_TRANSPORT | CLASS_VOLUME,     // ams
        CLASS_TRANSPORT | CLASS_VOLUME,     // hid
        CLASS_ALL,                          // res
        CLASS_TRANSPORT | CLASS_VOLUME,     // usb
    };

    // An action a sink has nothing for although its class is in: AMS has
    // volume up and down but no mute.
    inline constexpr bool destActs(uint8_t dest, Action a)
    {
        return !(dest == DEST_AMS && a == Action::Mute);
    }

    // Bit i set: destination i takes the key. `cls` is classOf(action); `isLong`
    // is whether the key's first edge is a Long (an asLong row).
    inline uint8_t keyDests(uint8_t cls, bool isLong, const DestState (&d)[DEST_COUNT])
    {
        if (cls == CLASS_NONE) return 0;
        uint8_t out = 0;
        for (uint8_t i = 0; i < DEST_COUNT; ++i) {
            if (!(cls & DEST_CLASSES[i])) continue;
            if (d[i].live && (d[i].mask & cls) && !(isLong && d[i].pressOnly))
                out |= (uint8_t)(1u << i);
        }
        return out;
    }

    // The same for one action: also drops the sinks that have no command for it.
    inline uint8_t keyDests(Action a, bool isLong, const DestState (&d)[DEST_COUNT])
    {
        uint8_t out = keyDests(classOf(a), isLong, d);
        for (uint8_t i = 0; i < DEST_COUNT; ++i)
            if (!destActs(i, a)) out &= (uint8_t)~(1u << i);
        return out;
    }

    // The volume fallback: a volume key that no live sink would take goes to
    // the phone over AMS, when AMS is live. With a head unit the user turns
    // the head unit's volume and the phone's stays put; without one (the UART
    // dest is not live while the head unit is silent) the keys are not dead.
    // Only the volume class falls back. True: add CLASS_VOLUME to AMS's mask.
    inline bool volumeToAms(const DestState (&d)[DEST_COUNT])
    {
        if (!d[DEST_AMS].live || (d[DEST_AMS].mask & CLASS_VOLUME)) return false;
        for (uint8_t i = 0; i < DEST_COUNT; ++i)
            if (i != DEST_AMS && d[i].live && (d[i].mask & DEST_CLASSES[i] & CLASS_VOLUME)) return false;
        return true;
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
