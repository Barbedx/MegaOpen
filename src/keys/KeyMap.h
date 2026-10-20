#pragma once
#include <stdint.h>
#include "KeyAction.h"

// What each of the 9 matrix positions means, plus the raw head-unit code to send.
// Portable: shared by the web UI, NVS and the native tests.
//
// Megane display connector (6 wires):
//   rows    = connector wires 4, 5, 6   (driven low one at a time)
//   columns = connector wires 1, 3, 2   (read with pull-ups)
// Factory layout (from a working implementation; re-assignable from /learn):
//
//   pos 0 (r0c0) VOL-      pos 1 (r0c1) MODE      pos 2 (r0c2) VOL+
//   pos 3 (r1c0) SOURCE    pos 4 (r1c1) RADIO     pos 5 (r1c2) MUTE
//   pos 6 (r2c0) enc A     pos 7 (r2c1) enc B     pos 8 (r2c2) enc C
namespace Keys
{
    struct Slot {
        Action  action = Action::None;
        uint8_t code   = 0;      // 0 = use the profile default for the action
    };

    class KeyMap {
    public:
        static constexpr uint8_t POSITIONS = 9;

        void loadDefaults();

        const Slot& slot(uint8_t pos) const { return _slots[pos < POSITIONS ? pos : 0]; }
        void setSlot(uint8_t pos, Slot s) { if (pos < POSITIONS) _slots[pos] = s; }

        // Positions marked EncPhaseA/B/C, in that order (0xFF when unset). Not
        // hard-coded to row 2: another keypad may wire the wheel elsewhere.
        void phases(uint8_t& a, uint8_t& b, uint8_t& c) const;

        // Bitmap of positions that need the fast debounce path (the phases).
        uint16_t fastMask() const;

        // Wheel direction codes; not per slot, since every phase yields both.
        uint8_t encCode(bool cw) const { return cw ? _encCw : _encCcw; }
        void setEncCode(bool cw, uint8_t code) { (cw ? _encCw : _encCcw) = code; }

        // Flat serialisation for NVS: 2 bytes per position + the two wheel codes.
        static constexpr uint8_t BLOB_SIZE = POSITIONS * 2 + 2;
        void toBlob(uint8_t* out) const;
        bool fromBlob(const uint8_t* in, uint8_t len);

    private:
        Slot    _slots[POSITIONS];
        uint8_t _encCw  = 0;   // 0 = profile default (RAV4 NEXT)
        uint8_t _encCcw = 0;   // 0 = profile default (RAV4 PREV)
    };
}
