#pragma once
#include "../vehicle/DecodeTable.h"
#include "../proto/canbox/FieldMap.h"
#include "../proto/canbox/Protocol.h"
#include "../vehicle/Formulas.h"
#include "../vehicle/Rules.h"

// The car's configuration on the board: /cfg/car.json in LittleFS (CarFile's
// document, by signal NAME), held in PSRAM while the board runs. NVS is 20 kB
// and keeps the small board settings only (Cfg::Config).
//
// Setup reads it once (begin, then load*); httpd writes it (save*), each save
// rewritten whole through /cfg/car.tmp, so a power cut leaves the old file or
// the new one, never half of one. One writer at a time (a mutex).
namespace CarStore
{
    // After the flash log mounted LittleFS (LogFile::begin). No file yet: a
    // new one, every part the factory's.
    void begin();
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

    // The rules; none in the file = the factory set (RuleSet::factory).
    void loadRules(Vehicle::RuleSet& set);
    bool saveRules(const Vehicle::RuleSet& set);
    // The factory set, written as rows.
    bool factoryRules();
}
