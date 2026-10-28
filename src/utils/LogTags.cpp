#include "LogTags.h"
#include <string.h>
#include <ctype.h>

namespace
{
    bool sameCi(const char* a, size_t an, const char* b)
    {
        const size_t bn = strlen(b);
        if (an != bn) return false;
        for (size_t i = 0; i < an; ++i)
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
        return true;
    }
    bool sep(char c) { return c == ' ' || c == ',' || c == ';' || c == '\t'; }
}

uint8_t LogTags::levelOf(const char* s, size_t n)
{
    static const char* const NAMES[5] = {"ERR", "WRN", "INF", "DBG", "TRC"};
    static const char LETTERS[5] = {'E', 'W', 'I', 'D', 'T'};
    for (uint8_t i = 0; i < 5; ++i) {
        if (sameCi(s, n, NAMES[i])) return i;
        if (n == 1 && toupper((unsigned char)s[0]) == LETTERS[i]) return i;
    }
    if (sameCi(s, n, "VERBOSE")) return 4;
    return NONE;
}

// The level the spec gives tag, NONE when it names no such tag.
uint8_t LogTags::specLevel(const char* tag) const
{
    const char* p = _spec;
    uint8_t found = NONE;
    while (*p) {
        while (*p && sep(*p)) ++p;
        const char* t = p;
        while (*p && *p != '=' && !sep(*p)) ++p;
        const size_t tn = (size_t)(p - t);
        if (*p != '=') { while (*p && !sep(*p)) ++p; continue; }
        const char* l = ++p;
        while (*p && !sep(*p)) ++p;
        if (tn && sameCi(t, tn, tag)) found = levelOf(l, (size_t)(p - l));   // the last one wins
    }
    return found;
}

int8_t LogTags::index(const char* tag)
{
    if (!tag || !*tag) return -1;
    for (uint8_t i = 0; i < _n; ++i)
        if (_name[i] == tag || !strcmp(_name[i], tag)) return (int8_t)i;
    if (_n >= MAX) return -1;
    _name[_n] = tag;
    _lvl[_n] = specLevel(tag);
    return (int8_t)_n++;
}

bool LogTags::setSpec(const char* spec)
{
    if (!spec) spec = "";
    if (strlen(spec) > SPEC_MAX) return false;
    // Every token must be tag=LVL with a known level.
    const char* p = spec;
    while (*p) {
        while (*p && sep(*p)) ++p;
        if (!*p) break;
        const char* t = p;
        while (*p && *p != '=' && !sep(*p)) ++p;
        if (*p != '=' || p == t) return false;
        const char* l = ++p;
        while (*p && !sep(*p)) ++p;
        if (levelOf(l, (size_t)(p - l)) == NONE) return false;
    }
    strncpy(_spec, spec, SPEC_MAX);
    _spec[SPEC_MAX] = 0;
    for (uint8_t i = 0; i < _n; ++i) _lvl[i] = specLevel(_name[i]);
    return true;
}
