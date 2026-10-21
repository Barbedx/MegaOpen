#pragma once
#include <stddef.h>
#include <stdint.h>
#include <ArduinoJson.h>
#include "CanDecode.h"
#include "../utils/UiList.h"

// The user's decode map: what POST /api/map edits, what NVS keeps (the blob
// `decode`, Config::saveDecode), and what the car task loads into CanDecode
// (staged, no reboot). Portable and native-tested (test_decode).
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
    inline constexpr UiName MAP_SRC[] = {{"can", "CAN A"}, {"const", "стала"}, {"off", "вимк"}};
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
        static constexpr uint8_t MAX_ROWS   = CanDecode::MAX_ROWS;   // 96
        static constexpr uint8_t MAX_CONSTS = 16;

        // THE BLOB, version 1, little-endian:
        //   [1] version  [1] rows  [1] constants
        //   per row, 23 B: id u32, byte, len, shift, mask u32, mul i16, div i16,
        //                  add i32, sig, flags, holdMs u16
        //   per constant, 5 B: sig, value i32
        static constexpr uint8_t ROW_BYTES   = 23;
        static constexpr uint8_t CONST_BYTES = 5;
        static constexpr size_t  BLOB_MAX    = 3 + MAX_ROWS * ROW_BYTES + MAX_CONSTS * CONST_BYTES;

        DecodeRow   row[MAX_ROWS];
        uint8_t     nRows = 0;
        DecodeConst k[MAX_CONSTS];
        uint8_t     nK = 0;

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
        // caller's: 2.4 kB, not for the stack) and copied back only when
        // every part fits. nullptr = applied; else one of the WHY_s, with
        // this untouched.
        static constexpr const char* WHY_ROWS   = "the map is full: 96 CAN rows";
        static constexpr const char* WHY_CONSTS = "the map is full: 16 constants";
        static constexpr const char* WHY_NO_CAN = "confirm: the signal has no CAN row to confirm";
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

        // The blob's bytes (at most BLOB_MAX) / back. unpack() checks what
        // CanDecode::load() would and the caps, and leaves the table empty
        // when it refuses.
        size_t pack(uint8_t* out) const;
        bool   unpack(const uint8_t* in, size_t n);
    };

    // The `can` object of /api/map, both ways: one writer, one reader.
    namespace DecodeJson
    {
        // {"id":"0x354","byte":0,"len":2,"shift":0,"mask":"FFFF","mul":1,
        //  "div":1,"add":0,"le":false,"signed":false,"hyp":true}; the length
        // written. `hyp`: not measured in the car (DF_HYPOTHESIS).
        int canJson(char* b, size_t cap, const DecodeRow& r);
        // The same keys back into `r` (canId..flags; sig and holdMs are the
        // caller's). An absent `hyp` is false. nullptr = ok, else why.
        const char* canFrom(JsonObjectConst o, DecodeRow& r);
    }
}
