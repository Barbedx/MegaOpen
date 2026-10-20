#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// UTF-8 clean-up ahead of affa::toAscii / affa::normalizeTitle, for what the
// library folds to '?': typographic apostrophes and quotes (Ukrainian "п’ятий"
// is written with U+2019 or U+02BC), dashes, the ellipsis, and emoji, which are
// dropped rather than shown as '?'. Runs of spaces left behind are collapsed and
// the ends trimmed. Everything else passes through untouched for the library.
// Header-only and Arduino-free so every env and the host tests get it as is.
namespace MegaOpen {

inline size_t preFold(const char* in, char* out, size_t cap)
{
    if (!out || !cap) return 0;
    size_t n = 0;
    auto put = [&](const char* s, size_t len) {
        for (size_t i = 0; i < len && n + 1 < cap; ++i) {
            if (s[i] == ' ' && (n == 0 || out[n - 1] == ' ')) continue;   // collapse
            out[n++] = s[i];
        }
    };
    const uint8_t* p = (const uint8_t*)(in ? in : "");
    while (*p) {
        uint32_t cp = 0;
        size_t len = 1;
        if (*p < 0x80)                { cp = *p; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = (*p & 0x1F) << 6 | (p[1] & 0x3F); len = 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = (*p & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F); len = 3; }
        else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = (uint32_t)(*p & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F);
            len = 4; }

        if (cp == 0x2018 || cp == 0x2019 || cp == 0x02BC || cp == 0x02B9 || cp == 0x00B4)
            put("'", 1);
        else if (cp == 0x201C || cp == 0x201D || cp == 0x00AB || cp == 0x00BB)
            put("\"", 1);
        else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212)
            put("-", 1);
        else if (cp == 0x2026)
            put("...", 3);
        else if (cp == 0x00A0)
            put(" ", 1);
        else if ((cp >= 0x1F000 && cp <= 0x1FAFF) ||   // emoji, flags, symbols
                 (cp >= 0x2600 && cp <= 0x27BF)  ||   // misc symbols, dingbats
                 (cp >= 0x2B00 && cp <= 0x2BFF)  ||   // arrows, stars
                 cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D || cp == 0x20E3)
            ;                                         // dropped
        else
            put((const char*)p, len);
        p += len;
    }
    while (n && out[n - 1] == ' ') --n;
    out[n] = '\0';
    return n;
}

}  // namespace MegaOpen
