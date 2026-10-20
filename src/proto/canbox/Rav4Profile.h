#pragma once
#include <stdint.h>
#include "../../keys/KeyAction.h"

// Toyota RAV4 2018-2020 key codes for the Raise canbox message set.
//
//   cmd 0x20, payload [code, state], state 0 = release, 1 = press, 2 = long
//
// Full documented set (docs/raise-protocol.md §5). Gaps (0x05, 0x06, 0x0C-0x12,
// 0x17-0x80) are unallocated; the unit discards them. Defaults only: the web UI
// can override any position's code.
namespace Canbox
{
    static constexpr uint8_t CMD_KEY = 0x20;

    enum : uint8_t {
        RAV4_NONE     = 0x00,
        RAV4_VOL_UP   = 0x01,
        RAV4_VOL_DOWN = 0x02,
        RAV4_RIGHT    = 0x03,   // the unit remaps this to "down" in its own UI
        RAV4_LEFT     = 0x04,   // ...and this to "up"
        RAV4_SOURCE   = 0x07,   // SRC / MODE; mute is a LONG press of this
        RAV4_SPEECH   = 0x08,
        RAV4_PICKUP   = 0x09,
        RAV4_HANGUP   = 0x0A,
        RAV4_HOME     = 0x0B,   // "box" key, added in V1.19.006
        RAV4_UP       = 0x13,
        RAV4_DOWN     = 0x14,
        RAV4_BACK     = 0x15,
        RAV4_ENTER    = 0x16,
        // 0x81-0x86: rotary; second byte is a detent count (1-255), no release.
        RAV4_KNOB_VOL_UP   = 0x81,
        RAV4_KNOB_VOL_DOWN = 0x82,
        RAV4_KNOB_CH_UP    = 0x83,
        RAV4_KNOB_CH_DOWN  = 0x84,
        RAV4_KNOB_TUNE_UP  = 0x85,
        RAV4_KNOB_TUNE_DOWN= 0x86,
        // Ordinary buttons.
        RAV4_PWR      = 0x87,
        RAV4_MODE     = 0x88,
    };

    enum : uint8_t { KEY_RELEASE = 0, KEY_PRESS = 1, KEY_LONG = 2 };

    inline constexpr bool isKnobCode(uint8_t code) {
        return code >= RAV4_KNOB_VOL_UP && code <= RAV4_KNOB_TUNE_DOWN;
    }

    // 0 when the action has no sensible default on this profile.
    uint8_t rav4CodeFor(Keys::Action a);
}
