#pragma once
// Small helpers the WebUi*.cpp route files share: request parameters and hex.
#include <Arduino.h>
#include <PsychicHttp.h>
#include "../utils/Hex.h"
#include "WebUi.h"

// serializeJson straight into a chunked reply: no String of the whole part.
struct ChunkWriter {
    ChunkOut& o;
    size_t write(uint8_t c) { const char ch = (char)c; o.put(&ch, 1); return 1; }
    size_t write(const uint8_t* b, size_t n) { o.put(reinterpret_cast<const char*>(b), n); return n; }
};

// ---- small shared helpers ---------------------------------------------------

// "7D 03 39 30" / "7d033930" -> bytes. -1 on a stray character or odd nibble
// count: a half-parsed payload would get a valid checksum over the wrong bytes.
static inline int parseHex(const char* s, uint8_t* out, size_t maxLen)
{
    size_t n = 0;
    int hi = -1;
    for (; *s; ++s) {
        if (*s == ' ' || *s == ',' || *s == ':' || *s == '-') continue;
        const int v = Hex::nibble(*s);
        if (v < 0) return -1;
        if (hi < 0) { hi = v; continue; }
        if (n >= maxLen) return -1;
        out[n++] = (uint8_t)((hi << 4) | v);
        hi = -1;
    }
    return hi >= 0 ? -1 : (int)n;
}

static inline void appendHex(String& s, const uint8_t* b, uint8_t n)
{
    char t[4];
    for (uint8_t i = 0; i < n; ++i) {
        snprintf(t, sizeof(t), "%02X", b[i]);
        if (i) s += ' ';
        s += t;
    }
}

static inline bool paramBool(PsychicRequest* r, const char* name, bool dflt = false)
{
    if (!r->hasParam(name)) return dflt;
    const String v = r->getParam(name)->value();
    return v == "1" || v == "true" || v == "on";
}

// By value, not const char*: .c_str() of PsychicHttp's temporary String dangles
// once the helper returns.
static inline String paramStr(PsychicRequest* r, const char* name, const char* dflt = "")
{
    return r->hasParam(name) ? r->getParam(name)->value() : String(dflt);
}

// Default given as text so it goes through the same parse as a browser value.
static inline uint32_t paramU32(PsychicRequest* r, const char* name, const char* dflt, int base)
{
    return (uint32_t)strtoul(paramStr(r, name, dflt).c_str(), nullptr, base);
}

// The menu's scroll byte outside the catalog's `arrows` (Catalog::arrowsKnown).
static const char kScrollWhy[] = "param scroll";   // 0, 7, 11 or 3
