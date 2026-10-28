#pragma once
#include <stddef.h>
#include <stdint.h>
#include <ArduinoJson.h>
#include "CanDecode.h"
#include "../utils/UiList.h"
#include "../utils/PsArray.h"

// The user's decode map: what POST /api/map edits, what the car file keeps
// (CarFile, by signal name), and what the car task loads into CanDecode
// (staged, no reboot). Portable and native-tested (test_decode). pack/unpack
// are 1.6.x's NVS blob: CarStore reads it once, to carry it into the file.
//
// A signal is bound one of three ways:
//   can    one or more CanDecode rows (the extraction in CanDecode.h);
//   const  a fixed value, applied as a pin (State: a pin wins over CAN);
//   off    neither: the emitter stays silent about it.
// decode_defaults.inc is the factory content, used when NVS holds no blob;
// "back to factory" erases the key.
namespace Vehicle
{
    // /api/map rows' `src`, served as the catalog's enums.mapSrc.
    inline constexpr UiName MAP_SRC[] = {{"can", "CAN A"}, {"const", nullptr}, {"off", nullptr}};
    enum MapSrc : uint8_t { SRC_CAN = 0, SRC_CONST, SRC_OFF };

    struct DecodeConst {
        uint8_t sig;
        int32_t value;
    };

    // One field of a POST /api/map, already checked: what binds `sig` now.
    struct DecodeEdit {
        uint8_t   sig;
        uint8_t   src;               // MapSrc
        int32_t   k;
        DecodeRow row;
    };

    // The map part of one POST /api/map: back to factory first, then the
    // edits, then the ✓ (on the new rows).
    struct DecodeChange {
        bool              reset   = false;
        const DecodeEdit* ed      = nullptr;
        uint8_t           n       = 0;
        bool              hasConf = false;
        uint8_t           confSig = 0;
        bool              confOn  = true;
    };

    struct DecodeTable {
        // Sanity caps, not sizes: the arrays grow as a car needs (PsArray,
        // in PSRAM). The factory has 62 rows.
        static constexpr uint16_t MAX_ROWS   = CanDecode::MAX_ROWS;   // 512
        static constexpr uint16_t MAX_CONSTS = 128;

        // 1.6.x's NVS blob (read once by CarStore), version 1, little-endian:
        //   [1] version  [1] rows  [1] constants
        //   per row, 23 B: id u32, byte, len, shift, mask u32, mul i16, div i16,
        //                  add i32, sig, flags, holdMs u16
        //   per constant, 5 B: sig, value i32
        static constexpr uint8_t ROW_BYTES   = 23;
        static constexpr uint8_t CONST_BYTES = 5;
        static constexpr uint8_t LEGACY_ROWS = 96, LEGACY_CONSTS = 16;   // its caps
        static constexpr size_t  BLOB_MAX    = 3 + LEGACY_ROWS * ROW_BYTES + LEGACY_CONSTS * CONST_BYTES;

        PsArray<DecodeRow>   row;   // sorted by id (CanDecode needs it)
        PsArray<DecodeConst> k;

        // decode_defaults.inc, no constants.
        void factory();
        // The first row / the constant bound to `sig`, or -1.
        int rowOf(uint8_t sig) const;
        int constOf(uint8_t sig) const;
        // Bound to CAN, and every row binding it is measured (no
        // DF_HYPOTHESIS): what Vehicle::verified() reports, pins aside.
        bool measured(uint8_t sig) const;
        // The owner's ✓ after a car check (POST /api/map {confirm}): every
        // row binding `sig` measured, or (↺) each row's factory flag back: a
        // row with a factory twin (sameField) takes the twin's, any other
        // row is a hypothesis. False, with nothing changed, when no CAN row
        // binds it. factory() brings the factory's flags back.
        bool setMeasured(uint8_t sig, bool on);

        // All or nothing: `c` is built on `scratch` (a copy of this, the
        // caller's; its rows are in PSRAM) and copied back only when
        // every part fits. nullptr = applied; else one of the WHY_s, with
        // this untouched.
        static constexpr const char* WHY_ROWS   = "map_full_rows 512";     // the map is full: 512 CAN rows
        static constexpr const char* WHY_CONSTS = "map_full_consts 128";   // the map is full: 128 constants
        static constexpr const char* WHY_NO_CAN = "confirm: no_can_row";   // the signal has no CAN row to confirm
        const char* apply(const DecodeChange& c, DecodeTable& scratch);
        // decode_defaults.inc binds `sig` and has measured it: a ✓ there is
        // the factory's, not one to take back.
        static bool factoryMeasured(uint8_t sig);
        // The same bytes, bits and scale: an edit that changes nothing a
        // measurement could have confirmed (the page re-saving a row).
        static bool sameField(const DecodeRow& a, const DecodeRow& b);
        // Replace whatever binds `sig`. False, with nothing changed, when the
        // table has no room. Rows stay sorted by id (CanDecode needs it).
        bool bindCan(uint8_t sig, const DecodeRow& r);
        bool bindConst(uint8_t sig, int32_t value);
        void unbind(uint8_t sig);
        // One more row (kept sorted) / constant, beside what binds its
        // signal already (a signal may have several rows). False: at the cap
        // or out of memory.
        bool addRow(const DecodeRow& r);
        bool addConst(uint8_t sig, int32_t value);
        void sortRows();

        // The 1.6.x blob's bytes (at most BLOB_MAX; 0 past its caps) / back.
        // unpack() checks what CanDecode::load() would and the caps, and
        // leaves the table empty when it refuses.
        size_t pack(uint8_t* out) const;
        bool   unpack(const uint8_t* in, size_t n);
    };

    // The `can` object of /api/map, both ways: one writer, one reader.
    namespace DecodeJson
    {
        // {"id":"0x354","byte":0,"len":2,"shift":0,"mask":"FFFF","mul":1,
        //  "div":1,"add":0,"le":false,"signed":false,"hyp":true,"latch":false,
        //  "key":false,"state2":0,"holdMs":0}: the whole row but its signal;
        // the length written. `hyp`: not measured in the car (DF_HYPOTHESIS);
        // state2 0 none, 1 DF_STATE2, 2 with DF_STATE2_INV.
        int canJson(char* b, size_t cap, const DecodeRow& r);
        // The same keys back into `r` (all but sig, the caller's). An absent
        // flag is off, an absent holdMs 0. nullptr = ok, else why.
        const char* canFrom(JsonObjectConst o, DecodeRow& r);
    }
}
