#pragma once
#include <stddef.h>
#include <stdint.h>
#include <ArduinoJson.h>
#include "../vehicle/DecodeTable.h"
#include "../proto/canbox/FieldMap.h"
#include "../proto/canbox/Protocol.h"
#include "../vehicle/Formulas.h"

// The car's configuration as one JSON document (docs/design/rules.md, slice 1):
// what /cfg/car.json in LittleFS holds (CarStore) and what a settings file
// carries (/api/config). Every reference to a signal is its NAME, so a
// firmware that adds, removes or reorders signals reads the same file; a row
// whose name this firmware does not know is skipped and counted, never
// guessed at.
//
//   {"v":1,
//    "decode":{"rows":[{"sig":"speed","can":{...DecodeJson::canJson...}}],
//              "consts":[{"sig":"batt_v","value":1250}]},
//    "hu":{"raise112":{"fields":[{"field":"range","from":"sig","src":"range_calc",
//                                 "mul":1,"div":1,"add":0}]}},
//    "formulas":[{"name":"fuel_pct","f":"fuel_level * 100 / 60","unit":"",
//                 "dec":0,"bool":false,"on":true}]}
//
// No `decode`: the factory table (decode_defaults.inc). Field maps are kept
// per protocol: another protocol's entry is left as it is.
//
// Portable: env:native tests it (test_carfile).
namespace CarFile
{
    constexpr int VERSION = 1;

    // ---- the decode table ---------------------------------------------------
    void putDecode(JsonObject car, const Vehicle::DecodeTable& t);
    // `car["decode"]` into `t` (emptied first). nullptr = read, with `skipped`
    // rows or constants whose signal is unknown; else why, `t` empty.
    const char* getDecode(JsonObjectConst car, Vehicle::DecodeTable& t, uint16_t& skipped);

    // ---- a protocol's field map -------------------------------------------------
    void putFields(JsonObject car, const Canbox::Protocol& p, const Canbox::FieldMap& m);
    // Absent: an empty map. nullptr = read (unknown fields or sources skipped
    // and counted); else why, `m` empty.
    const char* getFields(JsonObjectConst car, const Canbox::Protocol& p, Canbox::FieldMap& m, uint16_t& skipped);

    // ---- the formulas (Vehicle::FormulaSet) --------------------------------------
    void putFormulas(JsonObject car, const Vehicle::FormulaSet& set);
    // `car["formulas"]`, as written; checked by the caller (Formulas::check).
    // nullptr = read; "formulas: missing" = none (the caller's factory set).
    const char* getFormulas(JsonObjectConst car, Vehicle::FormulaSet& set);
    // One row both ways, for /api/formulas too.
    void        formulaRowTo(JsonObject o, const Vehicle::FormulaRow& r);
    const char* formulaRowFrom(JsonObjectConst o, Vehicle::FormulaRow& r);

    // ---- one field rule, as /api/hu/fields and the file write it ----------------
    // {"from":"own"|"sig"|"k"|"off","src","mul","div","add"}; the length written.
    int ruleJson(char* b, size_t cap, const Canbox::FieldRule& r);
    // The same back, for field `field`. `from` "auto" = no rule (`clear` set).
    // nullptr = ok, else why (an e.<code> with its key: "src: param").
    const char* ruleFrom(JsonObjectConst o, uint8_t field, Canbox::FieldRule& r, bool& clear);
}
