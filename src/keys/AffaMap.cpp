#include "AffaMap.h"
#include "../utils/UiList.h"

namespace Keys
{
    // Order is stored in NVS and used by the web UI: append only, never reorder.
    static const affa::Key kKeys[AffaMap::KEYS] = {
        affa::Key::Load,
        affa::Key::SrcNext,
        affa::Key::SrcPrev,
        affa::Key::VolUp,
        affa::Key::VolDown,
        affa::Key::Pause,
        affa::Key::RollUp,
        affa::Key::RollDown,
    };

    // Ids as the API stores them; names as /api/catalog shows them.
    static const UiName kKeyNames[AffaMap::KEYS] = {
        {"Load",     nullptr},
        {"SrcNext",  nullptr},
        {"SrcPrev",  nullptr},
        {"VolUp",    nullptr},
        {"VolDown",  nullptr},
        {"Pause",    nullptr},
        {"RollUp",   nullptr},
        {"RollDown", nullptr},
    };

    uint8_t AffaMap::indexOf(affa::Key k)
    {
        for (uint8_t i = 0; i < KEYS; ++i) if (kKeys[i] == k) return i;
        return KEYS;
    }

    affa::Key AffaMap::keyAt(uint8_t i) { return kKeys[i < KEYS ? i : 0]; }
    const char* AffaMap::keyName(uint8_t i) { return i < KEYS ? kKeyNames[i].id : "?"; }
    const char* AffaMap::keyTitle(uint8_t i) { return i < KEYS ? kKeyNames[i].name : "?"; }

    void AffaMap::loadDefaults()
    {
        for (auto& r : _rows) r = Row{};

        auto set = [this](uint8_t keyIdx, bool hold, Action a, bool asLong) {
            Row r; r.action = a; r.code = 0; r.asLong = asLong;
            _rows[rowIndex(keyIdx, hold)] = r;
        };

        set(0, false, Action::Load,    false);   // Load     -> ENTER
        set(1, false, Action::SrcNext, false);   // SrcNext  -> SOURCE
        set(2, false, Action::SrcPrev, false);   // SrcPrev  -> SOURCE
        set(3, false, Action::VolUp,   false);
        set(4, false, Action::VolDown, false);
        set(5, false, Action::Ok,      false);   // Pause    -> ENTER

        // Hold rows default to the same action sent as a long press.
        set(0, true, Action::Load,    true);
        set(1, true, Action::Mute,    false);    // hold SRC = MUTE, the OEM gesture
        set(2, true, Action::SrcPrev, true);
        set(3, true, Action::VolUp,   true);
        set(4, true, Action::VolDown, true);
        set(5, true, Action::Ok,      true);

        // Wheel: one detent = one event, no release.
        set(6, false, Action::EncCw,  false);
        set(7, false, Action::EncCcw, false);

        // Wheel hold rows stay empty: 0x0101|0xC0 and 0x0141|0xC0 are both 0x01C1
        // (0x40 is RollDown's direction bit and part of the hold mask), so a held
        // detent's direction is unknowable.
    }

    bool AffaMap::lookup(affa::Key k, bool hold, Row& out) const
    {
        const uint8_t i = indexOf(k);
        if (i >= KEYS) return false;
        const Row& r = _rows[rowIndex(i, hold)];
        if (r.action == Action::None) return false;
        out = r;
        return true;
    }

    void AffaMap::toBlob(uint8_t* out) const
    {
        for (uint8_t i = 0; i < ROWS; ++i) {
            out[i * 3 + 0] = (uint8_t)_rows[i].action;
            out[i * 3 + 1] = _rows[i].code;
            out[i * 3 + 2] = _rows[i].asLong ? 1 : 0;
        }
    }

    bool AffaMap::fromBlob(const uint8_t* in, uint8_t len)
    {
        if (!in || len != BLOB_SIZE) return false;
        for (uint8_t i = 0; i < ROWS; ++i) {
            const uint8_t a = in[i * 3 + 0];
            // Unknown action (newer firmware): reject the whole blob, not half of it.
            if (a >= (uint8_t)Action::_Count) return false;
            _rows[i].action = (Action)a;
            _rows[i].code   = in[i * 3 + 1];
            _rows[i].asLong = in[i * 3 + 2] != 0;
        }
        return true;
    }
}
