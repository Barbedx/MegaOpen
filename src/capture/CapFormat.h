#pragma once
#include "../utils/UiList.h"
#include <stddef.h>
#include <stdint.h>

// The capture file format and the change-only filter (docs/design/event-tasks.md,
// Step 5b). Portable on purpose: no Arduino, no FreeRTOS, so test/test_capture
// round-trips it on the host and tools/can_dump.py is checked against the same
// numbers. CanRecorder.cpp is the only firmware user.
//
// A FILE (/cap/NNNN.bin) is a 64-byte header followed by 18-byte records, all
// little-endian, no padding, no trailer. A file cut short by a power cut is
// still valid up to its last whole record.
//
// HEADER (64 bytes)
//    0  char[6] "MOCAP1"
//    6  u16     header length (64; a reader skips to it)
//    8  u32     session: the file number of the session's first file
//   12  u16     part: 0 for the session's first file, then 1, 2 ...
//   14  u8      mode at open (0 change-only, 1 full)
//   15  u8      record size (18)
//   16  u32[2]  bitrate of bus 0 (car) and bus 1 (panel), bps; 0 = unknown
//   24  u32     boot number (NVS sys/boots)
//   28  u32     wall clock, unix seconds, at t_ms = wallAtMs; 0 = not known
//   32  u32     wallAtMs
//   36  u16     keyframe interval, ms (5000)
//   38  u16     stats interval, ms (1000)
//   40  char[24] build id (MegaOpen::buildId()), NUL-padded
//
// RECORD (18 bytes)
//    0  u32 t_ms     uptime when the frame was pushed (millis())
//    4  u32 w        id | bus<<29 | ext<<31 for frame-like kinds (see kind)
//    8  u8  len      data length 0..8
//    9  u8  kind     Kind below
//   10  u8  data[8]  unused bytes are 0
//
// KINDS
//   FRAME     a frame: in change-only mode, first seen or its data/len changed;
//             in full mode, every frame.
//   KEYFRAME  change-only mode: an unchanged frame written because its id was
//             last written KEYFRAME_MS ago. Data is the frame's own.
//   MARKER    owner's text. w = markerNo (bits 0-15) | total text length
//             (bits 16-23) | piece index (bits 24-31); len = text bytes in
//             this piece (0..8); data = those bytes. A marker of N bytes is
//             ceil(N/8) consecutive pieces (one piece with len 0 for ""). The
//             pieces of one marker are always written together.
//   STATS     change-only mode, once per STATS_MS per id that was seen:
//             w = the id word; data = u32 frames seen in the interval, u16
//             interval ms, u16 of those frames whose data changed. So a rate
//             is recoverable though most repeats are not written.
//   GAP       frames lost to a full ring since the last GAP: data = u32 count.
//             w = 0. Where a GAP sits, the change-only state is uncertain.
//   BITRATE   bus w>>29 now runs at data = u32 bps (autobaud settled late).
//   MODE      the mode changed mid-file: data[0] = Mode.
namespace CapFormat
{
    constexpr size_t   HEADER_SIZE = 64;
    constexpr size_t   REC_SIZE    = 18;
    constexpr char     MAGIC[7]    = "MOCAP1";

    constexpr uint16_t KEYFRAME_MS = 5000;
    constexpr uint16_t STATS_MS    = 1000;
    constexpr size_t   MARK_TEXT_MAX = 120;

    enum Kind : uint8_t { FRAME = 0, KEYFRAME = 1, MARKER = 2, STATS = 3, GAP = 4, BITRATE = 5, MODE = 6 };
    enum Mode : uint8_t { MODE_CHANGE = 0, MODE_FULL = 1 };
    // The API's names for Mode (/api/cap, /api/catalog capModes); row i = Mode i.
    inline constexpr UiName MODE_NAMES[] = {{"change", nullptr}, {"full", nullptr}};

    constexpr uint32_t ID_MASK   = 0x1FFFFFFFu;
    constexpr uint8_t  BUS_SHIFT = 29;
    constexpr uint32_t EXT_BIT   = 1u << 31;

    inline uint32_t idWord(uint32_t id, bool ext, uint8_t bus)
    {
        return (id & ID_MASK) | ((uint32_t)(bus & 3) << BUS_SHIFT) | (ext ? EXT_BIT : 0);
    }
    inline uint32_t idOf(uint32_t w)  { return w & ID_MASK; }
    inline uint8_t  busOf(uint32_t w) { return (uint8_t)((w >> BUS_SHIFT) & 3); }
    inline bool     extOf(uint32_t w) { return (w & EXT_BIT) != 0; }

    struct Header {
        uint32_t session = 0;
        uint16_t part = 0;
        uint8_t  mode = MODE_CHANGE;
        uint32_t bitrate[2] = {0, 0};
        uint32_t boot = 0;
        uint32_t wallSec = 0;
        uint32_t wallAtMs = 0;
        uint16_t keyframeMs = KEYFRAME_MS;
        uint16_t statsMs = STATS_MS;
        char     build[24] = {0};
    };

    struct Rec {
        uint32_t t = 0;
        uint32_t w = 0;
        uint8_t  len = 0;
        uint8_t  kind = FRAME;
        uint8_t  data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    };

    void encodeHeader(const Header& h, uint8_t out[HEADER_SIZE]);
    // False when the magic is wrong or n is short.
    bool decodeHeader(const uint8_t* in, size_t n, Header& h);

    void encodeRec(const Rec& r, uint8_t out[REC_SIZE]);
    void decodeRec(const uint8_t in[REC_SIZE], Rec& r);

    // How many pieces a marker of this text takes (text cut at MARK_TEXT_MAX).
    size_t markerPieces(const char* text);
    // The marker's pieces into out (up to max); returns how many were written,
    // 0 when max is too small for the whole marker (never half a marker).
    size_t markerRecs(uint16_t markerNo, const char* text, uint32_t t, Rec* out, size_t max);
    // Reassembles one marker from its consecutive pieces. Returns the text
    // length, -1 when the pieces are not one whole marker. out is NUL-ended.
    int markerText(const Rec* recs, size_t n, char* out, size_t cap);

    inline Rec u32Rec(uint8_t kind, uint32_t t, uint32_t w, uint32_t v)
    {
        Rec r;
        r.t = t; r.w = w; r.kind = kind; r.len = 4;
        r.data[0] = (uint8_t)v; r.data[1] = (uint8_t)(v >> 8);
        r.data[2] = (uint8_t)(v >> 16); r.data[3] = (uint8_t)(v >> 24);
        return r;
    }

    // The change-only decision, per id word. The table is the caller's memory
    // (PSRAM on the T-2CAN), sized a power of two; it is never allocated here.
    // An id that finds the table full is written every time (full-mode behaviour for
    // that id only) and counted in overflow(): never silently lost.
    class ChangeFilter {
    public:
        struct Entry {
            uint32_t w;
            uint32_t lastWriteMs;
            uint32_t count;        // frames this stats interval
            uint16_t changes;      // of those, data changed
            uint8_t  len;
            uint8_t  used;         // 0 free, 1 seen, 2 written at least once
            uint8_t  data[8];
        };
        static constexpr uint8_t SKIP = 0xFF;

        // cap must be a power of two.
        void begin(Entry* table, uint16_t cap, uint16_t keyframeMs = KEYFRAME_MS);
        // Forget every id and its counts (a new session, a mode change).
        void reset();
        // A new file: every id's next frame is written as FRAME, so the file
        // decodes on its own; the ids and the interval's counts are kept, so
        // the next STATS record is still the whole interval.
        void rekey();

        // FRAME, KEYFRAME, or SKIP. Always counts the frame for STATS.
        uint8_t onFrame(const Rec& r);

        // True when STATS_MS has passed since the last emitStats().
        bool statsDue(uint32_t nowMs) const { return (uint32_t)(nowMs - _statsAt) >= STATS_MS; }
        // One STATS record per id seen since the last call, through out().
        // Returns how many. Resets the per-interval counts.
        using Out = void (*)(const Rec& r, void* ctx);
        size_t emitStats(uint32_t nowMs, Out out, void* ctx);
        // Restart the stats interval at nowMs without emitting.
        void startStats(uint32_t nowMs) { _statsAt = nowMs; }

        uint16_t ids() const      { return _ids; }
        uint32_t overflow() const { return _overflow; }

    private:
        Entry*   _t = nullptr;
        uint16_t _cap = 0;
        uint16_t _ids = 0;
        uint16_t _keyMs = KEYFRAME_MS;
        uint32_t _overflow = 0;
        uint32_t _statsAt = 0;
        Entry*   find(uint32_t w, bool insert);
    };
}
