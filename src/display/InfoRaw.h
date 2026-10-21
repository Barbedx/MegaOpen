#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// One info-list row with a declared length of our choosing, for the glass
// sweep (docs/glass-sweeps.md step 7): does the Carminat show more than the
// 8 cells the library's showInfoMenu sends?
//
//   10 <len> 76 <prefix> <offset> t0 .. t(len-4)
//
// len = 3 header bytes + the text cells; the library's is 0x0B (8 cells).
// Everything is clamped or refused here, so the frame is never longer than
// FRAME_MAX. DEV ONLY (/api/display/inforaw, refused without devMode): once
// the photos give the real cell count, showInfoMenu takes a count in the
// library and this goes (docs/BACKLOG.md).
//
// Pure: host-tested in test_glass.
namespace MegaOpen
{
    namespace InfoRaw
    {
        inline constexpr uint8_t LEN_MIN = 0x0B;        // the library's: 8 cells
        inline constexpr uint8_t LEN_MAX = 0x13;        // 16 cells
        inline constexpr uint8_t HEADER  = 3;           // 76 prefix offset
        inline constexpr uint8_t CELLS_MAX = LEN_MAX - HEADER;
        inline constexpr size_t  FRAME_MAX = 2 + LEN_MAX;
        // The OEM list uses offsets 0x41 / 0x44 / 0x48 and prefix 0x60.
        inline constexpr uint8_t OFFSET_MIN = 0x40, OFFSET_MAX = 0x4F;
        inline constexpr uint8_t PREFIX_MIN = 0x40, PREFIX_MAX = 0x7F;

        inline uint8_t clampLen(uint32_t len)
        {
            return len < LEN_MIN ? LEN_MIN : len > LEN_MAX ? LEN_MAX : (uint8_t)len;
        }
        inline bool offsetOk(uint32_t o) { return o >= OFFSET_MIN && o <= OFFSET_MAX; }
        inline bool prefixOk(uint32_t p) { return p >= PREFIX_MIN && p <= PREFIX_MAX; }

        // The frame into `out` (FRAME_MAX bytes); its length, or 0 when an
        // argument is out of range. `ascii` is the panel's charset already;
        // it is cut to the cells and space-padded, as the OEM list pads.
        inline size_t build(uint8_t* out, uint8_t len, uint8_t prefix, uint8_t offset,
                            const char* ascii)
        {
            if (len < LEN_MIN || len > LEN_MAX || !offsetOk(offset) || !prefixOk(prefix)) return 0;
            const uint8_t cells = (uint8_t)(len - HEADER);
            const size_t tl = ascii ? strlen(ascii) : 0;
            size_t n = 0;
            out[n++] = 0x10;            // ISO-TP first frame
            out[n++] = len;
            out[n++] = 0x76;            // info row
            out[n++] = prefix;
            out[n++] = offset;
            for (uint8_t i = 0; i < cells; ++i)
                out[n++] = i < tl ? (uint8_t)ascii[i] : (uint8_t)' ';
            return n;
        }
    }
}
