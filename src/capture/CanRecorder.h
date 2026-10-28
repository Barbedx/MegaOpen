#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../Features.h"
#include "../bus/Frame.h"

// The CAN capture recorder (docs/design/event-tasks.md, Step 5b): long captures
// recorded in the car, analysed on the PC afterwards (tools/can_dump.py).
//
// PRODUCERS (the car task, the panel task) call push(): a copy into a RAM ring
// under a short spinlock, never a wait, never an allocation. A full ring drops
// the frame and counts it; the file gets a GAP record saying how many.
//
// THE WRITER is the `capwr` task (priority 1, core 0). It drains the ring
// through the change-only filter (src/capture/CapFormat.h) into a 16 kB chunk,
// and writes the chunk to /cap/NNNN.bin only when it is full, on a marker, on
// stop/rotate, or when the web downloads the live file. Never on a timer: a
// flash write stalls both cores. The cost: a
// power cut loses what is still in the chunk (up to 16 kB, under a second on
// a busy bus); a marker is on flash as soon as it is made.
//
// FILES AND SESSIONS. Each boot (autostart) or start() is a new SESSION. A
// session's first file is opened lazily, at its first record, so a boot with a
// silent bus leaves nothing behind. A file closes at the file size (4 MB) and
// the session continues in the next number. The session id is the number of
// its first file, written in every file's header.
//
// PROTECTION is per session, on flash as an empty sidecar /cap/NNNN.keep next
// to every file of the session (no index file to corrupt: a boot rebuilds the
// table from the directory and the headers). A marker protects its session
// when auto-protect is on; /api/cap/protect sets or clears it by hand.
//
// THE RING ON FLASH. Captures get (littlefs size - the log's fixed 2 MB) x max
// fill % (85). Before each chunk the writer deletes the oldest unprotected file
// until the chunk fits; protected files go only when nothing else is left, and
// that is logged. The log never shrinks for a capture, nor the reverse.
//
// SETTINGS live in NVS namespace "cap", owned here: ring, fill, fileMB,
// autoProtect, mode, autostart; and "next", the next file number.
//
// [cap] LINE. capwr logs it itself, every 10 s, through LogBus; statusLine()
// gives the same text to anyone else who wants it.
namespace Capture
{
    // HU: the head unit's UART, a frame in records of 8 bytes (src/main.cpp
    // huRecord, tools/can_dump.py hu).
    enum Bus : uint8_t { CAR = 0, PANEL = 1, HU = 2 };

    // The recorder's settings. Outside the feature gate: the settings
    // registry describes them on every build (native tests included).
    // setSettings() clamps: fillPct 10..95, fileMB 1..8.
    struct Settings {
        bool    ring = true;          // delete the oldest when over the fill
        uint8_t fillPct = 85;         // of the space left to captures
        uint8_t fileMB = 4;           // a file closes at this size
        bool    autoProtect = true;   // a marker protects its session
        uint8_t mode = 0;             // CapFormat::Mode: 0 change-only, 1 full
        bool    autostart = true;     // record from boot
    };

#if FEATURE_CAPTURE
    // After LogFile::begin() (the recorder shares its littlefs mount). Loads
    // the settings, allocates the ring once, scans /cap, starts capwr, and
    // starts recording if autostart is on.
    void begin();
    // Any task, never blocks, no malloc. False when not recording or dropped.
    bool push(const Frame& f, uint8_t bus, uint32_t tMs);
    // A numbered marker with text (cut at 120 bytes), also logged. Any task.
    void mark(const char* text);
    // The bus's rate, for the next file's header and a BITRATE record now.
    void setBitrate(uint8_t bus, uint32_t bps);

    // "[cap] frames=.. written=.. dropped=.. file=.. free=..kB rate=..fps"
    void statusLine(char* buf, size_t n);

    // ---- control (the web; any task) -----------------------------------------
    Settings settings();
    // Clamped and saved to NVS; mode applies at once, the rest at the next chunk.
    void setSettings(const Settings& s);

    void start();                     // a new session; no-op while recording
    void stop();                      // writes out the chunk and closes the file
    void setMode(uint8_t mode);       // CapFormat::Mode, this session (not saved)
    // Writes out the chunk and waits for capwr (the shutdown handler, a live
    // download). False on timeout.
    bool flush(uint32_t timeoutMs);

    struct Status {
        bool     ready;               // begin() ran and littlefs is mounted
        bool     recording;
        uint8_t  mode;
        uint32_t frames, written, dropped, markers;
        uint32_t ringUsed, ringCap;   // slots
        uint32_t curFile;             // 0 = none open
        uint32_t curBytes;
        uint32_t session;
        uint32_t capBytes, budget;    // all /cap files; what they may use
        uint32_t fsFree;
        uint32_t rateFps;
        uint32_t bitrate[2];
        uint16_t ids;
        uint16_t files;
        uint32_t filterOverflow;
        const char* lastError;
    };
    Status status();

    struct FileInfo {
        uint32_t num, session, size, wallSec;
        uint16_t part;
        bool     prot, current;
    };
    // The files, oldest first, into out; returns how many there are in total.
    size_t list(FileInfo* out, size_t max);

    // Results for the web. NOT_FOUND / BUSY (the live file, or being read) / OK.
    enum class Res : uint8_t { OK, NOT_FOUND, BUSY, FAILED };
    Res      remove(uint32_t num);
    uint32_t removeUnprotected();
    Res      protect(uint32_t num, bool on);    // the file's whole session
    // Every /cap file, protected too; recording resumes in a new session if it
    // was on. Returns files deleted.
    uint32_t formatArea();

    // A download: the path to read, and a hold so the file is not deleted while
    // it streams. The live file is flushed first. endRead() always after.
    Res  beginRead(uint32_t num, char* path, size_t n);
    void endRead(uint32_t num);
#else
    inline void begin() {}
    inline bool push(const Frame&, uint8_t, uint32_t) { return false; }
    inline void mark(const char*) {}
    inline void setBitrate(uint8_t, uint32_t) {}
    inline void statusLine(char* buf, size_t n) { if (buf && n) buf[0] = 0; }
#endif
}
