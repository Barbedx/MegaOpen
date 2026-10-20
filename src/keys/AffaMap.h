#pragma once
#include <AffaDisplay.h>
#include <stdint.h>
#include "KeyAction.h"

// OEM stalk key map, indexed by AFFA wire code (KeyMap is the separate one
// indexed by matrix position; the two input paths compile independently).
// Sixteen rows: eight keys x click/hold.
//   * `action` — logical meaning; reaches every sink, including BLE.
//   * `code`   — literal RAV4 wire byte, or 0 = profile default for the action;
//     for RAV4 codes with no logical name.
namespace Keys
{
    class AffaMap {
    public:
        // The eight [REF]-attested stalk codes, fixed order. affa::Key is open;
        // other codes are counted in AffaKeySource::unmapped().
        static constexpr uint8_t KEYS = 8;
        static constexpr uint8_t EDGES = 2;               // 0 = click, 1 = hold
        static constexpr uint8_t ROWS = KEYS * EDGES;

        struct Row {
            Action  action = Action::None;   // None = this press is ignored entirely
            uint8_t code   = 0;              // 0 = the profile default for `action`
            bool    asLong = false;          // tell the head unit LONG rather than PRESS
        };

        void loadDefaults();

        // Wire code <-> row index. indexOf returns KEYS for an unknown code.
        static uint8_t indexOf(affa::Key k);
        static affa::Key keyAt(uint8_t i);
        static const char* keyName(uint8_t i);
        static const char* keyTitle(uint8_t i);     // Ukrainian, for /api/catalog
        // A held wheel detent has no wire form (0x0101|0xC0 == 0x0141|0xC0):
        // the library refuses to transmit one.
        static bool wireHold(affa::Key k) {
            return k != affa::Key::RollUp && k != affa::Key::RollDown;
        }

        static uint8_t rowIndex(uint8_t keyIdx, bool hold) {
            return (uint8_t)(keyIdx * EDGES + (hold ? 1 : 0));
        }

        const Row& row(uint8_t i) const { return _rows[i < ROWS ? i : 0]; }
        void setRow(uint8_t i, Row r) { if (i < ROWS) _rows[i] = r; }

        // False when the key is unknown or its row is empty.
        bool lookup(affa::Key k, bool hold, Row& out) const;

        // Flat serialisation for NVS: 3 bytes per row.
        static constexpr uint8_t BLOB_SIZE = ROWS * 3;
        void toBlob(uint8_t* out) const;
        bool fromBlob(const uint8_t* in, uint8_t len);

    private:
        Row _rows[ROWS];
    };
}
