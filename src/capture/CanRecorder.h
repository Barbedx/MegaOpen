#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../Features.h"
#include "../bus/Frame.h"
#include "../utils/UiList.h"

namespace Vehicle { class State; }

// The CAN capture recorder (docs/design/event-tasks.md, Step 5b; 1.12:
// docs/design/stabilisation-1-12.md S4): long captures recorded in the car,
// analysed on the PC afterwards (tools/can_dump.py).
//
// PRODUCERS (the car task, the panel task) call push(): a copy into a RAM
// queue under a short spinlock, never a wait, never an allocation. A full
// queue drops the frame and counts it; the capture gets a GAP record saying
// how many.
//
// THE WRITER is the `capwr` task (priority 1, core 0). It drains the queue
// through the change-only filter (src/capture/CapFormat.h) into v2 SEGMENTS
// in a RAM ring (PSRAM): a segment is sealed at 64 kB or 60 s, on a marker,
// on stop. Each one decodes on its own (its header, the id table, every id
// written whole first), so one lost never breaks another.
//
// SENDING. While online, the log push (src/net/LogPush.h, NetBudget's
// admission and the day's budget) takes the oldest sealed segment
// (takeSegment), PUTs it as cap/<session>/r<seq>.bin and frees it on 2xx or
// 422 (segmentDone). Nothing touches the flash for that.
//
// SPILLING. A segment no one sent goes to flash, /cap/<seq>.bin, in 4 kB
// pieces 2 ms apart (rule R4: no flash write stalls the cores long), when the
// ring would otherwise overwrite it; under the standing-only policy (the
// default, `cap.spill`) only while the car stands (speed 0 and the handbrake
// on, fresh), and then already when half the ring is unsent; moving, the
// oldest is overwritten and the next segment starts with a GAP of its
// records. save() (the web's «зберегти», refused while moving) and the
// reboot's flush() spill everything now. The flash files are what the log
// push sends next (NNNN-PP.bin), and what /api/cap/get downloads.
//
// THE LOSS BOUND. A power cut or a crash loses the open segment (60 s at
// most) and every sealed one still in RAM (the ring at most); statusLine()
// and status() say how much that is now (`unsent=n kB`).
//
// NUMBERS. Segments and files share one sequence ("next" in NVS), reserved 64
// at a time, so a reboot never hands out a number twice. Each boot
// (autostart) or start() is a new SESSION: its first segment's number.
//
// PROTECTION is per session, on flash as an empty sidecar /cap/NNNN.keep next
// to every file of the session (no index file to corrupt: a boot rebuilds the
// table from the directory and the headers). A marker protects its session
// when auto-protect is on; /api/cap/protect sets or clears it by hand.
//
// THE RING ON FLASH. Captures get (littlefs size - the log's fixed 2 MB) x max
// fill % (85). Before a spill the writer deletes the oldest unprotected file
// until the segment fits; protected files go only when nothing else is left,
// and that is logged. The log never shrinks for a capture, nor the reverse.
//
// SETTINGS live in NVS namespace "cap", owned here: ring, fill, fileMB (v1
// files only: segments are 64 kB), autoProtect, mode, autostart, ramMB (the
// RAM ring, read at boot), spill; and "next", the number sequence.
//
// [cap] LINE. capwr logs it itself, every 10 s, through LogBus; statusLine()
// gives the same text to anyone else who wants it.
namespace Capture
{
    // HU: the head unit's UART, a frame in records of 8 bytes (src/app/HuLink.cpp
    // huRecord, tools/can_dump.py hu).
    enum Bus : uint8_t { CAR = 0, PANEL = 1, HU = 2 };

    // When a segment no one sent may be written to flash (cap.spill).
    // STANDING: only while the car stands (SIG_SPEED 0, SIG_HANDBRAKE on);
    // PACED: whenever the ring would lose one, moving too. Both in 4 kB
    // pieces 2 ms apart. Standing is the default: a spill while driving costs
    // the panel and the keys whatever the flash stalls (unmeasured: S4 Verify).
    enum SpillPolicy : uint8_t { SPILL_STANDING = 0, SPILL_PACED = 1 };
    // The API's names (/api/catalog capSpill); row i = SpillPolicy i.
    inline constexpr UiName SPILL_NAMES[] = {{"standing", nullptr}, {"paced", nullptr}};

    // The recorder's settings. Outside the feature gate: the settings
    // registry describes them on every build (native tests included).
    // setSettings() clamps: fillPct 10..95, fileMB 1..8, ramMB 1..8.
    struct Settings {
        bool    ring = true;          // delete the oldest when over the fill
        uint8_t fillPct = 85;         // of the space left to captures
        uint8_t fileMB = 4;           // a file closes at this size
        bool    autoProtect = true;   // a marker protects its session
        uint8_t mode = 0;             // CapFormat::Mode: 0 change-only, 1 full
        bool    autostart = true;     // record from boot
        uint8_t ramMB = 2;            // the RAM ring asked for (next boot); PSRAM may give less
        uint8_t spill = SPILL_STANDING;
    };

#if FEATURE_CAPTURE
    // After LogFile::begin() (the recorder shares its littlefs mount). Loads
    // the settings, allocates the queue and the RAM ring once, scans /cap,
    // starts capwr, and starts recording if autostart is on.
    void begin();
    // The car's state, for the spill policy and save()'s refusal (main.cpp,
    // once). Null: never standing, never moving.
    void setVehicle(const Vehicle::State* s);
    // Speed 0 and the handbrake on, both fresh.
    bool standing();
    // Any task, never blocks, no malloc. False when not recording or dropped.
    bool push(const Frame& f, uint8_t bus, uint32_t tMs);
    // A numbered marker with text (cut at 120 bytes), also logged. Any task.
    void mark(const char* text);
    // The bus's rate, for the next segment's header and a BITRATE record now.
    void setBitrate(uint8_t bus, uint32_t bps);

    // "[cap] chg frames=.. written=.. seg=.. unsent=..kB B/rec=.. spill=.."
    void statusLine(char* buf, size_t n);

    // ---- control (the web; any task) -----------------------------------------
    Settings settings();
    // Clamped and saved to NVS; mode and spill apply at once, ramMB at the next boot.
    void setSettings(const Settings& s);

    void start();                     // a new session; no-op while recording
    void stop();                      // seals the segment (it stays in RAM to be sent)
    void setMode(uint8_t mode);       // CapFormat::Mode, this session (not saved)
    // The shutdown handler: seals and spills what RAM holds, paced, until
    // done or the timeout. False on timeout.
    bool flush(uint32_t timeoutMs);

    // Results for the web. NOT_FOUND / BUSY (being read) / MOVING (save()
    // while the car moves) / OK.
    enum class Res : uint8_t { OK, NOT_FOUND, BUSY, FAILED, MOVING };
    // The web's «зберегти»: seal and spill every segment in RAM now, in the
    // background (status().saving). MOVING: refused, the speed is above 0.
    // segs: how many go.
    Res save(uint32_t* segs = nullptr);

    // ---- the log push (src/net/LogPush.cpp) ------------------------------------
    struct SegInfo { uint32_t seq, session, len, records; uint16_t part; };
    // The oldest sealed segment nobody sent, copied into out (cap bytes, a
    // CapFormat::SEG_MAX is always enough); it is then "being sent". False:
    // none.
    bool takeSegment(uint8_t* out, size_t cap, SegInfo& info);
    // takeSegment's verdict: sent frees it; else it is sealed again (the
    // next push tries it again). A segment the ring took back meanwhile is
    // ignored.
    void segmentDone(uint32_t seq, bool sent);
    // Sealed segments waiting to be sent.
    uint32_t unsentSegments();

    struct Status {
        bool     ready;               // begin() ran and littlefs is mounted
        bool     recording;
        uint8_t  mode;
        uint32_t frames, written, dropped, markers;
        uint32_t ringUsed, ringCap;   // queue slots
        uint32_t curFile;             // the open segment's number; 0 = none
        uint32_t curBytes;            // its bytes
        uint32_t session;
        uint32_t capBytes, budget;    // all /cap files; what they may use
        uint32_t fsFree;
        uint32_t rateFps;
        uint32_t bitrate[2];
        uint16_t ids;
        uint16_t files;
        uint32_t filterOverflow;
        const char* lastError;
        // The RAM ring (S4).
        uint16_t segs;                // segments in the ring
        uint32_t segBytes;            // each
        uint16_t unsentSegs;          // sealed, not sent (or being sent / spilled)
        uint32_t unsentBytes;         // those and the open one: what a power cut loses now
        uint32_t sealed, sent, spilled, lostSegs, lostRecs;   // this boot
        uint32_t bytesPerRec100;      // the sealed segments' bytes per record, x100
        uint32_t spillMaxUs;          // the slowest spill piece this boot
        uint8_t  spill;               // SpillPolicy
        bool     standing, moving;
        bool     saving;              // a save() or a spill running
    };
    Status status();

    struct FileInfo {
        uint32_t num, session, size, wallSec;
        uint16_t part;
        bool     prot, current;
    };
    // The files, oldest first, into out; returns how many there are in total.
    size_t list(FileInfo* out, size_t max);

    Res      remove(uint32_t num);
    uint32_t removeUnprotected();
    Res      protect(uint32_t num, bool on);    // the file's whole session
    // Every /cap file, protected too, and the segments in RAM; recording
    // resumes in a new session if it was on. Returns files deleted.
    uint32_t formatArea();

    // A download: the path to read, and a hold so the file is not deleted while
    // it streams. endRead() always after.
    Res  beginRead(uint32_t num, char* path, size_t n);
    void endRead(uint32_t num);
#else
    inline void begin() {}
    inline void setVehicle(const Vehicle::State*) {}
    inline bool standing() { return false; }
    inline bool push(const Frame&, uint8_t, uint32_t) { return false; }
    inline void mark(const char*) {}
    inline void setBitrate(uint8_t, uint32_t) {}
    inline void statusLine(char* buf, size_t n) { if (buf && n) buf[0] = 0; }
#endif
}
