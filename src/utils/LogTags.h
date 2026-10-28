#pragma once
#include <stdint.h>
#include <stddef.h>

// The log's tags and their thresholds (docs/design/logging.md): no list kept by
// hand. A tag is added the first time a line uses it (LOGx caches the index per
// call site, so it is looked up once), up to MAX. A tag's threshold is its own
// override when the spec names it, else the global level.
//
// The spec is the setting `logTags`: "tag=LVL tag=LVL", LVL one of ERR WRN INF
// DBG TRC (case-insensitive), separated by spaces or commas. A tag the spec
// names before any line used it gets its level when it registers.
//
// Pure: no lock here (Log.cpp holds one around index()), tested in env:native.
// Tag names must outlive the table: string literals, as the macros pass.
class LogTags
{
public:
    static constexpr uint8_t MAX = 64;
    static constexpr uint8_t NONE = 0xFF;      // no override: the global level
    static constexpr size_t  SPEC_MAX = 160;

    // The tag's index, added when new; -1 when the table is full or tag is null/empty.
    int8_t   index(const char* tag);
    // The threshold for a line of tag i (-1: unknown tag) under `global`.
    uint8_t  threshold(int8_t i, uint8_t global) const
    {
        return i >= 0 && i < (int8_t)_n && _lvl[i] != NONE ? _lvl[i] : global;
    }
    // A new spec: every registered tag's override is set from it. False (and
    // nothing changes) on a token that is not tag=LVL, or one too long.
    bool     setSpec(const char* spec);
    const char* spec() const { return _spec; }

    uint8_t     count() const { return _n; }
    const char* name(uint8_t i) const { return i < _n ? _name[i] : ""; }
    uint8_t     override_(uint8_t i) const { return i < _n ? _lvl[i] : NONE; }

    // "ERR".."TRC" (or E W I D T) -> 0..4; NONE when not a level.
    static uint8_t levelOf(const char* s, size_t n);

private:
    uint8_t specLevel(const char* tag) const;

    const char* _name[MAX] = {};
    uint8_t     _lvl[MAX] = {};
    uint8_t     _n = 0;
    char        _spec[SPEC_MAX + 1] = "";
};
