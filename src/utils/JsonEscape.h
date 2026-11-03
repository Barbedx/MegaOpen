#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// One JSON string escape for every hand-built document (the log lines, the
// head-unit ring, the catalog): plain runs, UTF-8 included, go as they are;
// '"' and '\' get a backslash; a byte under 0x20 goes as \u00XX. Three copies
// used to exist and two of them let a control byte through, which is a
// document the page cannot parse. Pure: the host tests use it too.
namespace Json
{
    using Put = void (*)(const char* s, size_t n, void* ctx);

    // The body of a string (no quotes), handed to put() in runs.
    inline void escape(const char* s, size_t n, Put put, void* ctx)
    {
        const char* q = s;
        const char* const end = s + n;
        while (q < end) {
            const char* run = q;
            while (q < end && (uint8_t)*q >= 0x20 && *q != '"' && *q != '\\') ++q;
            if (q > run) put(run, (size_t)(q - run), ctx);
            if (q == end) break;
            char e[8];
            size_t k;
            if (*q == '"' || *q == '\\') { e[0] = '\\'; e[1] = *q; k = 2; }
            else k = (size_t)snprintf(e, sizeof(e), "\\u%04x", (unsigned)(uint8_t)*q);
            put(e, k, ctx);
            ++q;
        }
    }

    // With the quotes, into `out` (cap bytes, NUL-ended): the bytes written,
    // or 0 when it did not fit, out then "" (a cut string is still a
    // string, but a wrong one).
    inline size_t quoted(const char* s, size_t n, char* out, size_t cap)
    {
        struct Ctx { char* o; size_t cap, at; bool over; };
        Ctx c{out, cap, 0, false};
        auto put = [](const char* p, size_t k, void* v) {
            Ctx& x = *(Ctx*)v;
            if (x.over || x.at + k + 2 > x.cap) { x.over = true; return; }   // room for '"' and the NUL
            memcpy(x.o + x.at, p, k);
            x.at += k;
        };
        if (cap < 3) { if (cap) out[0] = 0; return 0; }
        out[c.at++] = '"';
        escape(s, n, put, &c);
        if (c.over) { out[0] = 0; return 0; }
        out[c.at++] = '"';
        out[c.at] = 0;
        return c.at;
    }
}
