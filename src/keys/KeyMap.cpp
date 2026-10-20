#include "KeyMap.h"

namespace Keys
{
    void KeyMap::loadDefaults()
    {
        _slots[0] = {Action::VolDown,   0};
        _slots[1] = {Action::Mode,      0};
        _slots[2] = {Action::VolUp,     0};
        _slots[3] = {Action::SrcNext,   0};
        _slots[4] = {Action::Radio,     0};
        _slots[5] = {Action::Mute,      0};
        _slots[6] = {Action::EncPhaseA, 0};
        _slots[7] = {Action::EncPhaseB, 0};
        _slots[8] = {Action::EncPhaseC, 0};
        _encCw = _encCcw = 0;
    }

    void KeyMap::phases(uint8_t& a, uint8_t& b, uint8_t& c) const
    {
        a = b = c = 0xFF;
        for (uint8_t i = 0; i < POSITIONS; ++i) {
            switch (_slots[i].action) {
                case Action::EncPhaseA: a = i; break;
                case Action::EncPhaseB: b = i; break;
                case Action::EncPhaseC: c = i; break;
                default: break;
            }
        }
    }

    uint16_t KeyMap::fastMask() const
    {
        uint16_t m = 0;
        for (uint8_t i = 0; i < POSITIONS; ++i)
            if (isEncoderPhase(_slots[i].action)) m |= (uint16_t)(1u << i);
        return m;
    }

    void KeyMap::toBlob(uint8_t* out) const
    {
        for (uint8_t i = 0; i < POSITIONS; ++i) {
            out[i * 2]     = (uint8_t)_slots[i].action;
            out[i * 2 + 1] = _slots[i].code;
        }
        out[POSITIONS * 2]     = _encCw;
        out[POSITIONS * 2 + 1] = _encCcw;
    }

    bool KeyMap::fromBlob(const uint8_t* in, uint8_t len)
    {
        if (!in || len != BLOB_SIZE) return false;
        for (uint8_t i = 0; i < POSITIONS; ++i) {
            uint8_t a = in[i * 2];
            if (a >= (uint8_t)Action::_Count) return false;   // corrupt/older layout
            _slots[i] = Slot{(Action)a, in[i * 2 + 1]};
        }
        _encCw  = in[POSITIONS * 2];
        _encCcw = in[POSITIONS * 2 + 1];
        return true;
    }
}
