#pragma once
#include "../vehicle/DecodeTable.h"
#include "../proto/canbox/FieldMap.h"
#include "../proto/canbox/Protocol.h"
#include "../vehicle/Formulas.h"

namespace Cfg { class Config; }

// The car's configuration on the board: /cfg/car.json in LittleFS (CarFile's
// document, by signal NAME), held in PSRAM while the board runs. Replaces the
// NVS blobs `decode` and `huFmap` (docs/design/rules.md, slice 1): NVS is
// 20 kB and keeps the small board settings only.
//
// Setup reads it once (begin, then load*); httpd writes it (save*), each save
// rewritten whole through /cfg/car.tmp, so a power cut leaves the old file or
// the new one, never half of one. One writer at a time (a mutex).
namespace CarStore
{
    // After the flash log mounted LittleFS (LogFile::begin). No file yet: it
    // is built once from what NVS held (1.6.x). Those keys are erased once
    // the file exists and this image is not on an OTA trial, so a rollback
    // during the trial still finds them.
    void begin(Cfg::Config& cfg);
    // Whether the file is the configuration's home on this boot (LittleFS
    // mounted, the document in memory). False: saves fail, loads are factory.
    bool ready();

    // The file's decode table, else the factory's. `from`: "file" or "factory".
    void loadDecode(Vehicle::DecodeTable& t, const char*& from);
    // The protocol's field map from the file; none = empty.
    void loadFields(const Canbox::Protocol& p, Canbox::FieldMap& m);

    // Into the document and the file. False: not written (no file system,
    // flash full, no memory); the running board has the change all the same.
    bool saveDecode(const Vehicle::DecodeTable& t);
    // Back to the factory table: the file's `decode` is dropped.
    bool factoryDecode();
    bool saveFields(const Canbox::Protocol& p, const Canbox::FieldMap& m);

    // The formulas; none in the file = the factory set (FormulaSet::factory).
    void loadFormulas(Vehicle::FormulaSet& set);
    bool saveFormulas(const Vehicle::FormulaSet& set);
    // Back to the factory set: the file's `formulas` is dropped.
    bool factoryFormulas();
}
