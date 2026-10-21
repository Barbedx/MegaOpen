#pragma once
#include <stdint.h>
#include "../bus/Frame.h"
#include "VehicleState.h"

// The car's frames into Vehicle::State (docs/design/event-tasks.md, Step 5).
// Portable and native-tested: frames and a time in, State writes out.
//
// One ROW per signal, in the extraction convention of docs/megane-can-ids.md:
//
//   raw   = the numBytes at startByte, big-endian (LE flag: little-endian)
//   field = (raw >> shift) & mask                 (SIGNED: two's complement
//                                                  in the mask's width)
//   value = field * mul / div + add               then clamped to the
//                                                  signal's range (Signals.h)
//
// ADDING A SIGNAL MEANS ADDING A ROW. The compiled defaults are in
// decode_defaults.inc; nothing else needs to change. Adding a Raise message is
// a builder plus a row in Rav4Emitter, on the other side of State.
//
// Rows are sorted by id and found by binary search; several rows may share an
// id (0x60D carries sixteen signals). A frame too short for a row skips that
// row and is counted, never read past its length.
//
// ONE WRITER: the car task. apply() writes State; nothing else decodes.
namespace Vehicle
{
    enum DecodeFlag : uint8_t {
        DF_SIGNED     = 1 << 0,  // the field is two's complement in the mask's width
        DF_LATCH      = 1 << 1,  // a non-zero value is held for holdMs after its last
                                 // sighting: indicators arrive as blink pulses, and a
                                 // plain decode would toggle the head unit's arrow
        DF_KEY        = 1 << 2,  // not a State value: each change goes to the key
                                 // callback (steering-wheel keys on the car's bus)
        DF_LE         = 1 << 3,  // little-endian bytes
        DF_EXT        = 1 << 4,  // the id is 29-bit; without it, 11-bit only
        DF_HYPOTHESIS = 1 << 5,  // not measured on our car yet: informational only,
                                 // decoded the same way (docs/megane-can-ids.md)
        DF_STATE2     = 1 << 6,  // a Renault 2-bit state (mask 0x3): 00 no, 01 yes,
                                 // 10/11 reserved or invalid, which leave the
                                 // signal untouched (it ages) and are counted
    };

    struct DecodeRow {   // 24 B: packed by field order
        uint32_t canId;
        uint8_t  startByte;
        uint8_t  numBytes;     // 1..4
        uint8_t  shift;
        uint32_t mask;         // after the shift: 1 for a bit, 0xFFFF for a word
        int16_t  mul;          // 16 bits: the row stays 24 B (DecodeTable keeps
        int16_t  div;          //   two copies), and a scale needs no more. 0 = 1
        int32_t  add;
        uint8_t  sig;          // Vehicle::Sig
        uint8_t  flags;        // DecodeFlag
        uint16_t holdMs;       // DF_LATCH only
    };

    class CanDecode {
    public:
        // The user's map (DecodeTable) is capped at this too; the factory has 62.
        static constexpr uint8_t MAX_ROWS = 96;

        // A DF_KEY row's value changed. Runs on the decoding task: post, never
        // block. No compiled row has the flag yet.
        using KeyFn = void (*)(uint8_t sig, int32_t value, void* ctx);

        // The table is NOT copied: `rows` must outlive this object (the
        // compiled defaults live in flash). It must be sorted by id; false,
        // and nothing loaded, when it is not or holds more than MAX_ROWS rows,
        // or a row names a signal or a width that does not exist.
        bool load(const DecodeRow* rows, uint8_t n);
        // decode_defaults.inc.
        bool loadDefaults();
        static const DecodeRow* defaults(uint8_t& n);

        void onKey(KeyFn fn, void* ctx) { _key = fn; _keyCtx = ctx; }

        // How many rows this frame matched (0 = an id the table does not know).
        uint8_t apply(const Frame& f, State& s, uint32_t nowMs);

        // One row's value from a payload; false when the payload is too short.
        static bool extract(const DecodeRow& r, const uint8_t* data, uint8_t len, int32_t& out);

        uint8_t          rows() const { return _n; }
        const DecodeRow& row(uint8_t i) const { return _r[i < _n ? i : 0]; }

        // Since boot, the decoding task's own.
        uint32_t hits() const    { return _hits; }     // frames that matched a row
        uint32_t unknown() const { return _unknown; }  // frames that matched none
        uint32_t short_() const  { return _short; }    // rows skipped: frame too short
        uint32_t invalid() const { return _invalid; }  // DF_STATE2 rows skipped: 10/11

    private:
        // The first row with this id, or _n.
        uint8_t lowerBound(uint32_t id) const;

        const DecodeRow* _r = nullptr;
        uint8_t  _n = 0;
        // Per row, the decoding task's own: the latch deadline and its value,
        // and a key row's last value.
        uint32_t _until[MAX_ROWS] = {};
        int32_t  _held[MAX_ROWS] = {};
        KeyFn    _key = nullptr;
        void*    _keyCtx = nullptr;
        uint32_t _hits = 0, _unknown = 0, _short = 0, _invalid = 0;
    };
}
