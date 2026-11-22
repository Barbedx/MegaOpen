#include "../app/TaskPlan.h"
#include "CanRecorder.h"
#if FEATURE_CAPTURE
#if !FEATURE_LOGFS
#error "FEATURE_CAPTURE writes to the littlefs that FEATURE_LOGFS mounts"
#endif
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "CapFormat.h"
#include "../Version.h"
#include "../app/Rt.h"
#include "../utils/LogBus.h"
#include "../utils/LogFile.h"
#include "../vehicle/VehicleState.h"

namespace Capture
{
    namespace
    {
        using namespace CapFormat;

        // ---- sizes ----------------------------------------------------------------
        constexpr size_t   RING_BYTES_PSRAM = 1024 * 1024;   // the queue: ~13 s of a full 500k bus
        constexpr size_t   RING_BYTES_SMALL = 32 * 1024;
        constexpr uint16_t IDS_PSRAM        = 1024;          // change-only table
        constexpr uint16_t IDS_SMALL        = 128;
        constexpr uint16_t MAX_FILES        = 128;
        // Room kept free beyond the budget for littlefs's own metadata and the
        // log's open block: the budget is a promise, not a hope.
        constexpr size_t   FS_SLACK         = 256 * 1024;
        constexpr uint32_t WAKE_MS          = 50;
        constexpr uint32_t LOG_MS           = 10000;
        constexpr uint32_t DRAIN_BATCH      = 4096;          // slots per pass, then beat

        // The RAM ring of v2 segments (S4). Sized at begin() from the PSRAM
        // left after the queue and the log push's spool, keeping PSRAM_KEEP
        // free for what comes and goes (PUT bodies, TLS in PSRAM, WiFi/lwIP).
        constexpr size_t   SEG_BYTES_PSRAM  = SEG_MAX;       // 64 kB
        constexpr size_t   SEG_BYTES_SMALL  = 8 * 1024;      // no PSRAM: two of these
        constexpr uint16_t SEGS_MAX         = 128;           // 8 MB at 64 kB
        constexpr size_t   PSRAM_KEEP       = 1536 * 1024;
        // A spill writes a segment to flash in pieces, a pause between (R4).
        constexpr size_t   SPILL_PIECE      = 4096;
        constexpr uint32_t SPILL_GAP_MS     = 2;
        // Numbers are reserved in NVS this many at a time: one write an hour
        // or so, never one per segment, and never a number used twice.
        constexpr uint32_t SEQ_RESERVE      = 64;
        // Speed and handbrake older than this are not known.
        constexpr uint32_t SIG_FRESH_MS     = 5000;

        // capwr: the plan's numbers (src/app/TaskPlan.h CAPWR_*).
        constexpr UBaseType_t CAPWR_PRIO  = App::Plan::CAPWR_PRIO;
        constexpr int         CAPWR_CORE  = App::Plan::CAPWR_CORE;
        constexpr uint32_t    CAPWR_STACK = App::Plan::CAPWR_STACK;

        // A queue slot: the frame as pushed. 20 bytes with alignment.
        struct Slot {
            uint32_t t;
            uint32_t w;
            uint8_t  len;
            uint8_t  kind;
            uint8_t  data[8];
        };

        // ---- the queue (producers + capwr) -----------------------------------------
        portMUX_TYPE      g_mux = portMUX_INITIALIZER_UNLOCKED;
        Slot*             g_ring = nullptr;
        uint32_t          g_cap = 0;
        uint32_t          g_head = 0;           // next free slot   (producers, under g_mux)
        uint32_t          g_tail = 0;           // oldest slot      (capwr, under g_mux)
        volatile uint32_t g_count = 0;          // slots in use     (both, under g_mux)
        volatile bool     g_rec = false;        // producers' gate
        volatile uint32_t g_frames = 0, g_dropped = 0, g_dropSinceGap = 0, g_markers = 0;
        uint16_t          g_markNo = 0;
        volatile uint32_t g_bitrate[2] = {0, 0};

        // ---- commands (any task -> capwr) --------------------------------------------
        enum : uint32_t { CMD_START = 1, CMD_STOP = 2, CMD_SYNC = 4, CMD_FORMAT = 8, CMD_MODE = 16, CMD_SAVE = 32 };
        volatile uint32_t g_cmdBits = 0;
        volatile uint32_t g_reqSeq = 0, g_doneSeq = 0;
        volatile uint8_t  g_modeReq = MODE_CHANGE;
        volatile uint32_t g_formatCount = 0;
        TaskHandle_t      g_task = nullptr;
        int               g_taskId = -1;

        // ---- the segments (g_mx for every field but the open one's bytes) ----------
        enum SegState : uint8_t { SEG_FREE, SEG_OPEN, SEG_SEALED, SEG_SENDING, SEG_SPILLING };
        struct Seg {
            uint8_t* buf;
            uint32_t len, seq, session, records, wallSec;
            uint16_t part;
            uint8_t  state;
        };
        Seg*         g_segs = nullptr;
        uint16_t     g_nSegs = 0;
        size_t       g_segBytes = 0;
        int          g_open = -1;               // capwr only
        uint32_t     g_openAtMs = 0;
        SegEncoder   g_enc;
        SegEncoder::Mem* g_encMem = nullptr;
        uint32_t     g_lostPending = 0;         // records of overwritten segments, for the next GAP
        volatile uint32_t g_sealed = 0, g_sent = 0, g_spilled = 0, g_lostSegs = 0, g_lostRecs = 0;
        volatile uint32_t g_sealedBytes = 0, g_sealedRecs = 0;   // the B/rec average (since boot)
        volatile uint32_t g_spillMaxUs = 0;     // the slowest piece of any spill this boot
        volatile bool     g_saveReq = false;    // spill every sealed segment up to g_saveUpTo
        uint32_t     g_saveUpTo = 0;

        // The spill in progress (capwr only).
        struct Spill {
            int      seg = -1;
            uint32_t off = 0, t0 = 0, maxUs = 0;
            uint16_t pieces = 0;
            bool     forced = false;
            File     f;
        };
        Spill g_sp;

        // ---- the writer (capwr only, unless noted) -----------------------------------
        uint32_t     g_session = 0;             // 0 = session pending its first segment
        uint16_t     g_part = 0;
        uint32_t     g_boot = 0;
        bool         g_active = false;          // a session is running
        bool         g_sessionProt = false;
        bool         g_markPending = false;     // a marker is in the segment: seal it
        volatile uint8_t g_mode = MODE_CHANGE;
        ChangeFilter g_filter;
        ChangeFilter::Entry* g_table = nullptr;
        volatile uint32_t g_written = 0;        // records
        volatile uint32_t g_rateFps = 0;
        bool         g_noRoomLogged = false;
        const Vehicle::State* volatile g_state = nullptr;

        // ---- the file table (g_mx) ------------------------------------------------------
        struct FileRec {
            uint32_t num, session, size, wallSec;
            uint16_t part;
            bool     prot;
            uint8_t  readers;
        };
        SemaphoreHandle_t g_mx = nullptr;
        // MAX_FILES records, allocated with the ring in begin() (PSRAM where
        // there is some): null before, when g_nFiles is 0 and no loop reads it.
        FileRec*  g_files = nullptr;
        uint16_t  g_nFiles = 0;
        uint32_t  g_next = 1;                   // the next number (segments and files)
        uint32_t  g_nextSaved = 1;              // NVS holds this: numbers below it are taken
        Settings  g_set;
        // A code, "" = none: the page has the words (web/i18n e.<code>).
        const char* volatile g_err = "";
        bool      g_ready = false;

        struct Lock {
            Lock()  { xSemaphoreTake(g_mx, portMAX_DELAY); }
            ~Lock() { xSemaphoreGive(g_mx); }
        };

        void binPath(uint32_t n, char* p, size_t cap)  { snprintf(p, cap, "/cap/%04lu.bin", (unsigned long)n); }
        void keepPath(uint32_t n, char* p, size_t cap) { snprintf(p, cap, "/cap/%04lu.keep", (unsigned long)n); }

        const char* spillName(uint8_t p) { return uiId(SPILL_NAMES, p); }

        // ---- settings ------------------------------------------------------------------
        Settings clampSettings(Settings s)
        {
            if (s.fillPct < 10) s.fillPct = 10;
            if (s.fillPct > 95) s.fillPct = 95;
            if (s.fileMB < 1) s.fileMB = 1;
            if (s.fileMB > 8) s.fileMB = 8;
            if (s.mode > MODE_FULL) s.mode = MODE_CHANGE;
            if (s.ramMB < 1) s.ramMB = 1;
            if (s.ramMB > 8) s.ramMB = 8;
            if (s.spill > SPILL_PACED) s.spill = SPILL_STANDING;
            return s;
        }

        void loadSettings()
        {
            Preferences p;
            p.begin("cap", true);
            Settings s;
            s.ring        = p.getBool("ring", s.ring);
            s.fillPct     = p.getUChar("fill", s.fillPct);
            s.fileMB      = p.getUChar("fileMB", s.fileMB);
            s.autoProtect = p.getBool("autoprot", s.autoProtect);
            s.mode        = p.getUChar("mode", s.mode);
            s.autostart   = p.getBool("auto", s.autostart);
            s.ramMB       = p.getUChar("ramMB", s.ramMB);
            s.spill       = p.getUChar("spill", s.spill);
            g_next        = p.getUInt("next", 1);
            p.end();
            g_nextSaved = g_next;
            g_set = clampSettings(s);
        }

        // ---- the car's state: may the flash be written now? -----------------------------
        bool standingNow()
        {
            const Vehicle::State* st = g_state;
            if (!st) return false;
            const uint32_t now = millis();
            return st->fresh(Vehicle::SIG_SPEED, now, SIG_FRESH_MS) && st->get(Vehicle::SIG_SPEED) == 0 &&
                   st->fresh(Vehicle::SIG_HANDBRAKE, now, SIG_FRESH_MS) && st->get(Vehicle::SIG_HANDBRAKE) != 0;
        }

        bool movingNow()
        {
            const Vehicle::State* st = g_state;
            return st && st->fresh(Vehicle::SIG_SPEED, millis(), SIG_FRESH_MS) && st->get(Vehicle::SIG_SPEED) > 0;
        }

        // A spill the policy allows now. forced: an explicit save (the web,
        // a reboot), which the policy does not hold.
        bool spillAllowed(bool forced)
        {
            return forced || g_set.spill == SPILL_PACED || standingNow();
        }

        // ---- the table (callers hold g_mx) --------------------------------------------
        int indexOf(uint32_t num)
        {
            for (uint16_t i = 0; i < g_nFiles; ++i) if (g_files[i].num == num) return i;
            return -1;
        }

        uint32_t capBytesLocked()
        {
            uint32_t s = 0;
            for (uint16_t i = 0; i < g_nFiles; ++i) s += g_files[i].size;
            return s;
        }

        // Sorted by number: the oldest first.
        void insertLocked(const FileRec& f)
        {
            if (!g_files || g_nFiles >= MAX_FILES) return;
            uint16_t i = g_nFiles;
            while (i > 0 && g_files[i - 1].num > f.num) { g_files[i] = g_files[i - 1]; --i; }
            g_files[i] = f;
            ++g_nFiles;
        }

        bool deleteLocked(uint16_t i)
        {
            char p[24];
            binPath(g_files[i].num, p, sizeof(p));
            const bool ok = !LittleFS.exists(p) || LittleFS.remove(p);
            keepPath(g_files[i].num, p, sizeof(p));
            if (LittleFS.exists(p)) LittleFS.remove(p);
            if (!ok) return false;
            for (uint16_t k = i; k + 1 < g_nFiles; ++k) g_files[k] = g_files[k + 1];
            --g_nFiles;
            return true;
        }

        bool deletable(const FileRec& f) { return f.readers == 0; }

        void setProtLocked(uint32_t session, bool on)
        {
            char p[24];
            for (uint16_t i = 0; i < g_nFiles; ++i) {
                FileRec& f = g_files[i];
                if (f.session != session || f.prot == on) continue;
                keepPath(f.num, p, sizeof(p));
                if (on) { File k = LittleFS.open(p, "w"); if (k) k.close(); }
                else if (LittleFS.exists(p)) LittleFS.remove(p);
                f.prot = on;
            }
        }

        // LittleFS.totalBytes() asks esp_littlefs_info(), which walks the whole
        // filesystem for the used figure as well: read once at begin().
        size_t g_fsTotal = 0;

        uint32_t budgetBytes()
        {
            const size_t total = g_fsTotal;
            const size_t keep = LogFile::RESERVED_BYTES + FS_SLACK;
            if (total <= keep) return 0;
            return (uint32_t)((uint64_t)(total - keep) * g_set.fillPct / 100);
        }

        // The oldest file the ring may take: unprotected first; a protected one
        // only when allowProt and nothing else is left. -1 when none.
        int victimLocked(bool allowProt)
        {
            for (uint16_t i = 0; i < g_nFiles; ++i)
                if (!g_files[i].prot && deletable(g_files[i])) return i;
            if (allowProt)
                for (uint16_t i = 0; i < g_nFiles; ++i)
                    if (deletable(g_files[i])) return i;
            return -1;
        }

        // Frees space (and a table row) for one more file of `need` bytes.
        // False: nothing may be deleted.
        bool makeRoom(uint32_t need)
        {
            const uint32_t budget = budgetBytes();
            Lock l;
            for (;;) {
                if (capBytesLocked() + need <= budget && g_nFiles < MAX_FILES) return true;
                if (!g_set.ring) { g_err = "cap_full_noring"; return false; }   // full, ring is off
                const int v = victimLocked(true);
                if (v < 0) { g_err = g_nFiles >= MAX_FILES ? "cap_files" : "cap_full"; return false; }
                const FileRec f = g_files[v];
                if (!deleteLocked((uint16_t)v)) { g_err = "cap_delete"; return false; }
                if (f.prot)
                    LOGI("cap", "deleted PROTECTED %04lu (session %04lu): nothing unprotected left",
                                   (unsigned long)f.num, (unsigned long)f.session);
                else
                    LOGI("cap", "ring: deleted %04lu (%lu kB)", (unsigned long)f.num,
                                   (unsigned long)(f.size / 1024));
            }
        }

        // ---- the boot scan ------------------------------------------------------------
        bool parseNum(const char* name, const char* ext, uint32_t& num)
        {
            // "NNNN.bin" / "NNNN.keep"; the core may hand back "/cap/NNNN.bin".
            const char* s = strrchr(name, '/');
            s = s ? s + 1 : name;
            char* end = nullptr;
            const unsigned long n = strtoul(s, &end, 10);
            if (end == s || n == 0 || strcmp(end, ext) != 0) return false;
            num = (uint32_t)n;
            return true;
        }

        void scan()
        {
            if (!LittleFS.exists("/cap")) LittleFS.mkdir("/cap");
            uint32_t keeps[MAX_FILES];
            uint16_t nKeeps = 0;
            uint32_t maxNum = 0;
            uint16_t emptied = 0;

            File dir = LittleFS.open("/cap");
            if (!dir || !dir.isDirectory()) return;
            for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
                const String name = e.name();
                uint32_t num = 0;
                if (parseNum(name.c_str(), ".keep", num)) {
                    if (nKeeps < MAX_FILES) keeps[nKeeps++] = num;
                    e.close();
                    continue;
                }
                if (!parseNum(name.c_str(), ".bin", num)) { e.close(); continue; }
                if (num > maxNum) maxNum = num;
                FileRec f = {};
                f.num = num;
                f.size = (uint32_t)e.size();
                uint8_t hb[HEADER2_SIZE];
                Header h;
                const size_t got = e.read(hb, sizeof(hb));
                e.close();
                // v1 (1.11 and before) or v2 (a spilled segment).
                if (!decodeHeader(hb, got, h) || f.size <= h.headerLen) {
                    // A header and nothing else (power cut at the first chunk)
                    // or not ours: deleted, so it never pins the ring.
                    char p[24];
                    binPath(num, p, sizeof(p));
                    LittleFS.remove(p);
                    ++emptied;
                    continue;
                }
                f.session = h.session;
                f.part = h.part;
                f.wallSec = h.wallSec;
                Lock l;
                insertLocked(f);
            }
            dir.close();

            Lock l;
            for (uint16_t k = 0; k < nKeeps; ++k) {
                const int i = indexOf(keeps[k]);
                if (i >= 0) g_files[i].prot = true;
                else { char p[24]; keepPath(keeps[k], p, sizeof(p)); LittleFS.remove(p); }
            }
            // A number is never reused while a file with it can exist.
            if (g_next <= maxNum) g_next = maxNum + 1;
            if (emptied) LOGI("cap", "removed %u empty capture file(s)", (unsigned)emptied);
        }

        // ---- numbers ------------------------------------------------------------------
        // The next segment number. NVS holds a number above every one handed
        // out: a reboot starts there, so a pushed rNNNN never comes again (a
        // 422 would free a segment GitHub does not have).
        uint32_t nextSeq()
        {
            if (g_next >= g_nextSaved) {
                g_nextSaved = g_next + SEQ_RESERVE;
                Preferences p;
                p.begin("cap", false);
                p.putUInt("next", g_nextSaved);
                p.end();
            }
            return g_next++;
        }

        uint32_t wallNow(uint32_t& atMs)
        {
            atMs = millis();
            const time_t t = time(nullptr);
            return t >= 1700000000 ? (uint32_t)t : 0;
        }

        // ---- segments (capwr) ------------------------------------------------------
        int oldestLocked(uint8_t state, uint32_t upTo = 0xFFFFFFFFu)
        {
            int best = -1;
            for (uint16_t i = 0; i < g_nSegs; ++i)
                if (g_segs[i].state == state && g_segs[i].seq <= upTo &&
                    (best < 0 || g_segs[i].seq < g_segs[best].seq))
                    best = i;
            return best;
        }

        uint16_t countLocked(uint8_t state)
        {
            uint16_t n = 0;
            for (uint16_t i = 0; i < g_nSegs; ++i) if (g_segs[i].state == state) ++n;
            return n;
        }

        // A sealed segment the ring takes back unsent: lost, and said so in
        // the next segment (a GAP of its records) and the [cap] line.
        void dropLocked(Seg& s)
        {
            ++g_lostSegs;
            g_lostRecs += s.records;
            g_lostPending += s.records;
            s.state = SEG_FREE;
        }

        void spillEndLocked(bool ok);

        // Starts writing segment i to /cap/<seq>.bin. False: no room (the
        // segment is then lost) or no file.
        bool spillStart(int i, bool forced)
        {
            Seg s;
            { Lock l; s = g_segs[i]; }
            if (!makeRoom(s.len)) {
                if (!g_noRoomLogged) {
                    LOGI("cap", "spill r%04lu dropped: %s", (unsigned long)s.seq, (const char*)g_err);
                    g_noRoomLogged = true;
                }
                Lock l;
                if (g_segs[i].state == SEG_SEALED) dropLocked(g_segs[i]);
                return false;
            }
            g_noRoomLogged = false;
            char p[24];
            binPath(s.seq, p, sizeof(p));
            g_sp.f = LittleFS.open(p, "w");
            if (!g_sp.f) {
                g_err = "cap_create";
                Lock l;
                if (g_segs[i].state == SEG_SEALED) dropLocked(g_segs[i]);
                return false;
            }
            Lock l;
            g_segs[i].state = SEG_SPILLING;
            g_sp.seg = i;
            g_sp.off = 0;
            g_sp.t0 = millis();
            g_sp.maxUs = 0;
            g_sp.pieces = 0;
            g_sp.forced = forced;
            return true;
        }

        // One piece; true when the spill is over.
        bool spillStep()
        {
            if (g_sp.seg < 0) return true;
            Seg& s = g_segs[g_sp.seg];   // SPILLING: nobody else touches it
            const uint32_t n = s.len - g_sp.off < SPILL_PIECE ? s.len - g_sp.off : (uint32_t)SPILL_PIECE;
            const uint32_t t0 = micros();
            const size_t w = n ? g_sp.f.write(s.buf + g_sp.off, n) : 0;
            uint32_t us = micros() - t0;
            if (us > g_sp.maxUs) g_sp.maxUs = us;
            ++g_sp.pieces;
            g_sp.off += (uint32_t)w;
            const bool failed = w != n;
            if (!failed && g_sp.off < s.len) return false;
            // The last piece: close commits littlefs's metadata, timed with it.
            const uint32_t c0 = micros();
            g_sp.f.close();
            us = micros() - c0;
            if (us > g_sp.maxUs) g_sp.maxUs = us;
            if (failed) g_err = "cap_write";   // short write: filesystem full?
            Lock l;
            spillEndLocked(!failed);
            return true;
        }

        void spillEndLocked(bool ok)
        {
            Seg& s = g_segs[g_sp.seg];
            if (g_sp.maxUs > g_spillMaxUs) g_spillMaxUs = g_sp.maxUs;
            if (ok) {
                FileRec f = {};
                f.num = s.seq; f.session = s.session; f.part = s.part; f.wallSec = s.wallSec;
                f.size = s.len;
                insertLocked(f);
                if (g_sessionProt && s.session == g_session) setProtLocked(s.session, true);
                ++g_spilled;
                LOGI("cap", "spilled r%04lu: %lu kB in %u piece(s), %lu ms, slowest %lu us (%s%s)",
                     (unsigned long)s.seq, (unsigned long)(s.len / 1024), (unsigned)g_sp.pieces,
                     (unsigned long)(millis() - g_sp.t0), (unsigned long)g_sp.maxUs,
                     g_sp.forced ? "saved, " : "", spillName(g_set.spill));
                s.state = SEG_FREE;
            } else {
                char p[24];
                binPath(s.seq, p, sizeof(p));
                LittleFS.remove(p);
                dropLocked(s);
                LOGI("cap", "spill r%04lu failed: %s", (unsigned long)s.seq, (const char*)g_err);
            }
            g_sp.seg = -1;
        }

        // The rest of a spill now, paced as ever (capwr waits; the queue holds).
        void spillFinish()
        {
            while (!spillStep()) vTaskDelay(pdMS_TO_TICKS(SPILL_GAP_MS));
        }

        // A slot for the next segment: a free one; else the oldest sealed one,
        // spilled when the policy allows it now, else overwritten (lost).
        int reclaim()
        {
            for (int pass = 0; pass < 3; ++pass) {
                int victim = -1;
                {
                    Lock l;
                    for (uint16_t i = 0; i < g_nSegs; ++i) if (g_segs[i].state == SEG_FREE) return i;
                    victim = oldestLocked(SEG_SEALED);
                    if (victim < 0 && g_sp.seg < 0) {
                        // All are being sent: take the oldest; its sender has a copy.
                        const int s = oldestLocked(SEG_SENDING);
                        if (s >= 0) { g_segs[s].state = SEG_FREE; return s; }
                        return -1;
                    }
                    if (victim >= 0 && g_sp.seg < 0 && !spillAllowed(false)) {
                        dropLocked(g_segs[victim]);
                        return victim;
                    }
                }
                if (g_sp.seg >= 0) { spillFinish(); continue; }
                if (victim >= 0 && spillStart(victim, false)) spillFinish();
            }
            return -1;
        }

        void sealSeg()
        {
            if (g_open < 0) return;
            const uint32_t len = (uint32_t)g_enc.size(), recs = g_enc.records();
            g_enc.close();
            Lock l;
            Seg& s = g_segs[g_open];
            s.len = len;
            s.records = recs;
            s.state = recs ? SEG_SEALED : SEG_FREE;
            if (recs) {
                ++g_sealed;
                g_sealedBytes += len;
                g_sealedRecs += recs;
            }
            g_open = -1;
        }

        bool openSeg(uint32_t t)
        {
            const int i = reclaim();
            if (i < 0) { g_err = "cap_ram"; return false; }
            const uint32_t seq = nextSeq();
            if (!g_session) { g_session = seq; g_part = 0; }
            else ++g_part;
            Header h;
            h.session = g_session;
            h.part = g_part;
            h.seq = seq;
            h.t0 = t;
            h.mode = g_mode;
            h.bitrate[0] = g_bitrate[0];
            h.bitrate[1] = g_bitrate[1];
            h.boot = g_boot;
            h.wallSec = wallNow(h.wallAtMs);
            strncpy(h.build, MegaOpen::buildId(), sizeof(h.build) - 1);
            {
                Lock l;
                Seg& s = g_segs[i];
                s.seq = seq; s.session = g_session; s.part = g_part; s.wallSec = h.wallSec;
                s.len = 0; s.records = 0;
                s.state = SEG_OPEN;
            }
            g_enc.open(g_segs[i].buf, g_segBytes, h);
            g_open = i;
            g_openAtMs = millis();
            // Each segment starts with every id written whole, so it decodes on its own.
            g_filter.rekey();
            if (g_lostPending) {
                g_enc.add(u32Rec(GAP, t, 1, g_lostPending));
                g_lostPending = 0;
            }
            return true;
        }

        void append(const Rec& r)
        {
            if (g_open < 0 && !openSeg(r.t)) return;
            if (!g_enc.add(r)) {
                sealSeg();
                if (!openSeg(r.t) || !g_enc.add(r)) return;
            }
            ++g_written;
        }

        void statsOut(const Rec& r, void*) { append(r); }

        // Seal, and spill every sealed segment up to now in the background.
        void requestSave()
        {
            sealSeg();
            Lock l;
            g_saveUpTo = g_next;   // above every number handed out
            g_saveReq = true;
        }

        // The spill policy, once a pass: start one when it is time.
        void spillPolicy()
        {
            if (g_sp.seg >= 0) return;
            int pick = -1;
            bool forced = false;
            {
                Lock l;
                if (g_saveReq) {
                    pick = oldestLocked(SEG_SEALED, g_saveUpTo);
                    if (pick >= 0) forced = true;
                    else g_saveReq = false;
                }
                if (pick < 0) {
                    const uint16_t sealed = countLocked(SEG_SEALED);
                    if (!sealed) return;
                    const uint16_t freeN = countLocked(SEG_FREE);
                    // The next segment would overwrite one that was never sent.
                    const bool full = freeN == 0;
                    // Standing-only: a stop with half the ring unsent is the
                    // chance to save it before the car moves again.
                    const bool chance = g_set.spill == SPILL_STANDING && sealed * 2 >= g_nSegs;
                    if (!full && !chance) return;
                    if (!spillAllowed(false)) return;
                    pick = oldestLocked(SEG_SEALED);
                }
            }
            if (pick >= 0) spillStart(pick, forced);
        }

        void beginSession()
        {
            g_session = 0;
            g_part = 0;
            g_sessionProt = false;
            g_markPending = false;
            g_active = true;
            g_filter.reset();
            g_enc.reset();
            g_filter.startStats(millis());
            g_rec = true;
        }

        void endSession()
        {
            sealSeg();
            g_active = false;
            g_session = 0;
        }

        void handle(const Slot& s)
        {
            Rec r;
            r.t = s.t; r.w = s.w; r.len = s.len; r.kind = s.kind;
            memcpy(r.data, s.data, 8);
            if (r.kind == FRAME) {
                if (g_mode == MODE_FULL) { append(r); return; }
                const uint8_t k = g_filter.onFrame(r);
                if (k != ChangeFilter::SKIP) { r.kind = k; append(r); }
                return;
            }
            append(r);
            if (r.kind == MARKER) g_markPending = true;
        }

        // Up to DRAIN_BATCH slots; true when the queue still holds more.
        bool drain()
        {
            uint32_t lost;
            portENTER_CRITICAL(&g_mux);
            lost = g_dropSinceGap;
            g_dropSinceGap = 0;
            const uint32_t n = g_count < DRAIN_BATCH ? g_count : DRAIN_BATCH;
            portEXIT_CRITICAL(&g_mux);

            if (lost && g_active) append(u32Rec(GAP, millis(), 0, lost));

            uint32_t t = g_tail;
            for (uint32_t i = 0; i < n; ++i) {
                if (g_active) handle(g_ring[t]);
                if (++t == g_cap) t = 0;
            }
            portENTER_CRITICAL(&g_mux);
            g_tail = t;
            g_count -= n;
            const bool more = g_count > 0;
            portEXIT_CRITICAL(&g_mux);
            return more;
        }

        void drainAll() { while (drain()) {} }

        void protectCurrentSession()
        {
            if (g_sessionProt || !g_session) return;
            g_sessionProt = true;
            Lock l;
            setProtLocked(g_session, true);
            LOGI("cap", "session %04lu protected (marker)", (unsigned long)g_session);
        }

        uint32_t formatLocked()
        {
            uint32_t n = 0;
            for (int i = (int)g_nFiles - 1; i >= 0; --i)
                if (deletable(g_files[i]) && deleteLocked((uint16_t)i)) ++n;
            return n;
        }

        uint32_t unsentLocked(uint16_t* segs)
        {
            uint32_t b = 0;
            uint16_t n = 0;
            for (uint16_t i = 0; i < g_nSegs; ++i) {
                const Seg& s = g_segs[i];
                if (s.state == SEG_SEALED || s.state == SEG_SENDING || s.state == SEG_SPILLING) { b += s.len; ++n; }
            }
            if (segs) *segs = n;
            return b;
        }

        void logLine()
        {
            static int8_t s_capTag = -1;
            if (!Log::statusOn("cap", s_capTag, millis())) return;
            char b[280];
            statusLine(b, sizeof(b));
            // statusLine() writes "[cap] ...": the tag comes from the logger.
            Log::write(LogLevel::INF, "cap", "%s", strncmp(b, "[cap] ", 6) ? b : b + 6);
        }

        void run(void*)
        {
            uint32_t logAt = millis();
            uint32_t framesAt = g_frames;
            for (;;) {
                // A spill goes on in pieces, SPILL_GAP_MS apart (R4).
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(g_sp.seg >= 0 ? SPILL_GAP_MS : WAKE_MS));
                const uint32_t t0 = micros();

                uint32_t cmd, seq;
                portENTER_CRITICAL(&g_mux);
                cmd = g_cmdBits;
                g_cmdBits = 0;
                seq = g_reqSeq;
                portEXIT_CRITICAL(&g_mux);

                if (cmd & CMD_STOP) {
                    g_rec = false;
                    drainAll();
                    if (g_active) {
                        endSession();
                        LOGI("cap", "stopped");
                    }
                }
                if (cmd & CMD_FORMAT) {
                    const bool was = g_active;
                    g_rec = false;
                    drainAll();
                    endSession();
                    if (g_sp.seg >= 0) spillFinish();
                    uint32_t n;
                    {
                        Lock l;
                        n = formatLocked();
                        // What RAM held goes too: nothing of the area is left.
                        for (uint16_t i = 0; i < g_nSegs; ++i)
                            if (g_segs[i].state == SEG_SEALED) g_segs[i].state = SEG_FREE;
                        g_saveReq = false;
                    }
                    g_formatCount = n;
                    g_err = "";
                    LOGI("cap", "capture area formatted: %lu file(s) deleted", (unsigned long)n);
                    if (was) beginSession();
                }
                if ((cmd & CMD_START) && !g_active) {
                    beginSession();
                    LOGI("cap", "recording (%s)", g_mode == MODE_FULL ? "full" : "change-only");
                }
                if ((cmd & CMD_MODE) && g_modeReq != g_mode) {
                    drainAll();
                    g_mode = g_modeReq;
                    g_filter.reset();
                    if (g_active && g_open >= 0) {
                        Rec r = u32Rec(MODE, millis(), 0, g_mode);
                        r.len = 1;
                        append(r);
                    }
                }
                if (cmd & (CMD_SYNC | CMD_SAVE)) {
                    drainAll();
                    requestSave();
                }

                bool more = false;
                if (g_active || g_count) more = drain();
                if (g_active && g_mode == MODE_CHANGE && g_filter.statsDue(millis()))
                    g_filter.emitStats(millis(), statsOut, nullptr);
                // 60 s at most: the segment can go out (pushed, or spilled).
                if (g_open >= 0 && g_enc.records() && (uint32_t)(millis() - g_openAtMs) >= SEG_MS) sealSeg();

                if (g_markPending) {
                    g_markPending = false;
                    sealSeg();   // the marker's segment can be pushed now
                    if (g_set.autoProtect) protectCurrentSession();
                }
                spillPolicy();
                if (g_sp.seg >= 0) spillStep();
                g_doneSeq = seq;

                const uint32_t now = millis();
                if ((uint32_t)(now - logAt) >= LOG_MS) {
                    const uint32_t f = g_frames;
                    g_rateFps = (uint32_t)((uint64_t)(f - framesAt) * 1000 / (now - logAt));
                    framesAt = f;
                    logAt = now;
                    if (g_active || g_rateFps) logLine();
                }
                if (g_taskId >= 0) App::Tasks::beat(g_taskId, micros() - t0);
                if (more) xTaskNotifyGive(xTaskGetCurrentTaskHandle());
            }
        }

        // Posts commands and waits until capwr has handled them. False on timeout.
        bool command(uint32_t bits, uint32_t timeoutMs)
        {
            if (!g_task) return false;
            uint32_t my;
            portENTER_CRITICAL(&g_mux);
            g_cmdBits |= bits;
            my = ++g_reqSeq;
            portEXIT_CRITICAL(&g_mux);
            xTaskNotifyGive(g_task);
            if (xTaskGetCurrentTaskHandle() == g_task) return false;
            const uint32_t t0 = millis();
            while ((int32_t)(g_doneSeq - my) < 0) {
                if ((uint32_t)(millis() - t0) >= timeoutMs) return false;
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            return true;
        }

        uint8_t clean(char c) { return (c >= 0x20 && c < 0x7F) ? (uint8_t)c : (uint8_t)'?'; }

        // The RAM ring: as many segments as PSRAM spares, keeping PSRAM_KEEP
        // free. Logs what PSRAM had, so the ring's size is explained.
        void allocSegments(bool ps)
        {
            const uint32_t caps = ps ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            g_segBytes = ps ? SEG_BYTES_PSRAM : SEG_BYTES_SMALL;
            uint16_t want = 2;
            if (ps) {
                const size_t freeB = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                const size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
                LOGI("mem", "psram free=%u big=%u (after the capture queue %u kB and the push spool)",
                     (unsigned)freeB, (unsigned)big, (unsigned)(RING_BYTES_PSRAM / 1024));
                const size_t asked = (size_t)g_set.ramMB * 1024 * 1024;
                const size_t avail = freeB > PSRAM_KEEP ? freeB - PSRAM_KEEP : 0;
                const size_t bytes = asked < avail ? asked : avail;
                want = (uint16_t)(bytes / g_segBytes > SEGS_MAX ? SEGS_MAX : bytes / g_segBytes);
            }
            g_segs = (Seg*)heap_caps_malloc(sizeof(Seg) * (want ? want : 1), caps);
            g_nSegs = 0;
            if (!g_segs) return;
            for (uint16_t i = 0; i < want; ++i) {
                uint8_t* b = (uint8_t*)heap_caps_malloc(g_segBytes, caps);
                if (!b) break;
                g_segs[i] = Seg{b, 0, 0, 0, 0, 0, 0, SEG_FREE};
                ++g_nSegs;
            }
            if (g_nSegs >= 2) return;
            for (uint16_t i = 0; i < g_nSegs; ++i) heap_caps_free(g_segs[i].buf);
            heap_caps_free(g_segs);
            g_segs = nullptr;
            g_nSegs = 0;
        }
    }

    // ---- public ---------------------------------------------------------------------

    void begin()
    {
        if (g_ready) return;
        if (!g_mx) g_mx = xSemaphoreCreateMutex();
        loadSettings();
        g_mode = g_set.mode;
        g_modeReq = g_mode;
        if (!LogFile::fsMounted()) {
            g_err = "cap_no_fs";   // littlefs not mounted
            LOGI("cap", "off: littlefs not mounted");
            return;
        }

        const bool ps = psramFound();
        const uint32_t caps = ps ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t ringBytes = ps ? RING_BYTES_PSRAM : RING_BYTES_SMALL;
        const uint16_t ids = ps ? IDS_PSRAM : IDS_SMALL;
        g_ring   = (Slot*)heap_caps_malloc(ringBytes, caps);
        g_table  = (ChangeFilter::Entry*)heap_caps_malloc(sizeof(ChangeFilter::Entry) * ids, caps);
        g_files  = (FileRec*)heap_caps_malloc(sizeof(FileRec) * MAX_FILES, caps);
        g_encMem = (SegEncoder::Mem*)heap_caps_malloc(sizeof(SegEncoder::Mem), caps);
        if (g_ring && g_table && g_files && g_encMem) allocSegments(ps);
        if (!g_ring || !g_table || !g_files || !g_encMem || !g_segs) {
            heap_caps_free(g_ring); heap_caps_free(g_table);
            heap_caps_free(g_files); heap_caps_free(g_encMem);
            g_ring = nullptr; g_table = nullptr; g_files = nullptr; g_encMem = nullptr;
            g_err = "no_memory";   // for the ring
            LOGI("cap", "off: no memory for the ring");
            return;
        }
        g_cap = (uint32_t)(ringBytes / sizeof(Slot));
        g_fsTotal = LittleFS.totalBytes();
        g_filter.begin(g_table, ids);
        g_enc.begin(g_encMem);
        {
            Preferences s;
            s.begin("sys", true);
            g_boot = s.getUInt("boots", 0);
            s.end();
        }

        scan();

        App::TaskSpec spec;
        spec.name  = "capwr";
        spec.fn    = run;
        spec.stack = CAPWR_STACK;
        spec.prio  = CAPWR_PRIO;
        spec.core  = CAPWR_CORE;
        spec.idOut = &g_taskId;
        g_task = App::spawn(spec);
        if (!g_task) { g_err = "cap_task"; return; }   // capwr did not start
        g_ready = true;

        uint32_t capB, nF;
        { Lock l; capB = capBytesLocked(); nF = g_nFiles; }
        LOGI("cap", "ready: queue %u kB (%s), RAM ring %u x %u kB = %u kB (asked %u MB), %lu file(s) %lu kB of %lu kB "
                    "budget, next %04lu, %s, autostart %s",
             (unsigned)(ringBytes / 1024), ps ? "psram" : "internal", (unsigned)g_nSegs, (unsigned)(g_segBytes / 1024),
             (unsigned)(g_nSegs * g_segBytes / 1024), (unsigned)g_set.ramMB, (unsigned long)nF,
             (unsigned long)(capB / 1024), (unsigned long)(budgetBytes() / 1024), (unsigned long)g_next,
             g_mode == MODE_FULL ? "full" : "change-only", g_set.autostart ? "on" : "off");
        LOGI("cap", "spill policy %s: %s; %u B pieces, %u ms apart",
             spillName(g_set.spill),
             g_set.spill == SPILL_PACED ? "flash written whenever the ring would lose a segment"
                                        : "flash written only standing (speed 0, handbrake on), else the oldest is overwritten",
             (unsigned)SPILL_PIECE, (unsigned)SPILL_GAP_MS);
        if (g_set.autostart) start();
    }

    void setVehicle(const Vehicle::State* s) { g_state = s; }
    bool standing() { return standingNow(); }

    bool push(const Frame& f, uint8_t bus, uint32_t tMs)
    {
        if (!g_rec) return false;
        const uint32_t w = idWord(f.id, f.extended, bus);
        const uint8_t len = f.len > 8 ? 8 : f.len;
        portENTER_CRITICAL(&g_mux);
        if (g_count >= g_cap) {
            ++g_dropped;
            ++g_dropSinceGap;
            portEXIT_CRITICAL(&g_mux);
            return false;
        }
        Slot& s = g_ring[g_head];
        s.t = tMs; s.w = w; s.len = len; s.kind = FRAME;
        memcpy(s.data, f.data, 8);
        if (++g_head == g_cap) g_head = 0;
        ++g_count;
        ++g_frames;
        portEXIT_CRITICAL(&g_mux);
        return true;
    }

    void mark(const char* text)
    {
        char t[MARK_TEXT_MAX + 1];
        size_t n = 0;
        if (text) for (; text[n] && n < MARK_TEXT_MAX; ++n) t[n] = (char)clean(text[n]);
        t[n] = 0;

        Rec recs[MARK_TEXT_MAX / 8 + 1];
        const uint32_t now = millis();
        bool stored = false;
        uint16_t no;
        portENTER_CRITICAL(&g_mux);
        no = ++g_markNo;
        if (g_rec) {
            const size_t k = markerRecs(no, t, now, recs, sizeof(recs) / sizeof(recs[0]));
            if (k && g_count + k <= g_cap) {
                for (size_t i = 0; i < k; ++i) {
                    Slot& s = g_ring[g_head];
                    s.t = recs[i].t; s.w = recs[i].w; s.len = recs[i].len; s.kind = recs[i].kind;
                    memcpy(s.data, recs[i].data, 8);
                    if (++g_head == g_cap) g_head = 0;
                }
                g_count += k;
                ++g_markers;
                stored = true;
            }
        }
        portEXIT_CRITICAL(&g_mux);
        if (g_task) xTaskNotifyGive(g_task);
        LOGI("cap", "mark #%u \"%s\"%s session=%04lu", (unsigned)no, t,
                       stored ? "" : (g_rec ? " NOT RECORDED (ring full)" : " (not recording)"),
                       (unsigned long)g_session);
    }

    void setBitrate(uint8_t bus, uint32_t bps)
    {
        if (bus > 1 || g_bitrate[bus] == bps) return;
        g_bitrate[bus] = bps;
        if (!g_rec) return;
        // Ordered with the frames: through the queue like them.
        portENTER_CRITICAL(&g_mux);
        if (g_count < g_cap) {
            Slot& s = g_ring[g_head];
            const Rec r = u32Rec(BITRATE, millis(), (uint32_t)bus << BUS_SHIFT, bps);
            s.t = r.t; s.w = r.w; s.len = r.len; s.kind = r.kind;
            memcpy(s.data, r.data, 8);
            if (++g_head == g_cap) g_head = 0;
            ++g_count;
        }
        portEXIT_CRITICAL(&g_mux);
    }

    void statusLine(char* buf, size_t n)
    {
        if (!buf || !n) return;
        if (!g_ready) { snprintf(buf, n, "[cap] off: %s", (const char*)g_err); return; }
        uint32_t capB, unsent;
        uint16_t unsentN;
        uint32_t openSeq = 0, openBytes = 0;
        {
            Lock l;
            capB = capBytesLocked();
            unsent = unsentLocked(&unsentN);
            if (g_open >= 0) { openSeq = g_segs[g_open].seq; openBytes = (uint32_t)g_enc.size(); }
        }
        const uint32_t budget = budgetBytes();
        const uint32_t freeK = budget > capB ? (budget - capB) / 1024 : 0;
        const uint32_t ringPct = g_cap ? (uint32_t)((uint64_t)g_count * 100 / g_cap) : 0;
        const uint32_t recs = g_sealedRecs, bytes = g_sealedBytes;
        const uint32_t bpr100 = recs ? (uint32_t)((uint64_t)bytes * 100 / recs) : 0;
        // unsent: what a power cut would lose now (the open tail and every
        // sealed segment not yet pushed or spilled).
        snprintf(buf, n, "[cap] %s frames=%lu written=%lu dropped=%lu seg=r%04lu(%lukB) unsent=%lukB(%u/%u) "
                         "B/rec=%lu.%02lu sent=%lu spilled=%lu lost=%lu spill=%s/%luus free=%lukB rate=%lufps "
                         "q=%lu%% ids=%u car=%lu%s%s",
                 g_rec ? (g_mode == MODE_FULL ? "full" : "chg") : "stopped",
                 (unsigned long)g_frames, (unsigned long)g_written, (unsigned long)g_dropped,
                 (unsigned long)openSeq, (unsigned long)(openBytes / 1024),
                 (unsigned long)((unsent + openBytes) / 1024), (unsigned)unsentN, (unsigned)g_nSegs,
                 (unsigned long)(bpr100 / 100), (unsigned long)(bpr100 % 100),
                 (unsigned long)g_sent, (unsigned long)g_spilled, (unsigned long)g_lostSegs,
                 spillName(g_set.spill), (unsigned long)g_spillMaxUs,
                 (unsigned long)freeK, (unsigned long)g_rateFps, (unsigned long)ringPct,
                 (unsigned)g_filter.ids(), (unsigned long)g_bitrate[0],
                 ((const char*)g_err)[0] ? " err=" : "", (const char*)g_err);
    }

    Settings settings()
    {
        if (!g_mx) return g_set;
        Lock l;
        return g_set;
    }

    void setSettings(const Settings& in)
    {
        const Settings s = clampSettings(in);
        Settings was;
        if (g_mx) { Lock l; was = g_set; g_set = s; } else { was = g_set; g_set = s; }
        Preferences p;
        p.begin("cap", false);
        p.putBool("ring", s.ring);
        p.putUChar("fill", s.fillPct);
        p.putUChar("fileMB", s.fileMB);
        p.putBool("autoprot", s.autoProtect);
        p.putUChar("mode", s.mode);
        p.putBool("auto", s.autostart);
        p.putUChar("ramMB", s.ramMB);
        p.putUChar("spill", s.spill);
        p.end();
        if (was.spill != s.spill) LOGI("cap", "spill policy %s", spillName(s.spill));
        if (was.ramMB != s.ramMB) LOGI("cap", "RAM ring %u MB from the next boot", (unsigned)s.ramMB);
        setMode(s.mode);
    }

    void start()  { if (g_ready) command(CMD_START, 1000); }
    void stop()
    {
        if (!g_ready) return;
        g_rec = false;   // producers stop now; capwr seals what came before
        command(CMD_STOP, 2000);
    }
    void setMode(uint8_t mode)
    {
        if (!g_ready || mode > MODE_FULL) return;
        g_modeReq = mode;
        command(CMD_MODE, 1000);
    }

    // The shutdown handler: seal, spill what RAM holds, as far as the time goes.
    bool flush(uint32_t timeoutMs)
    {
        if (!g_ready) return false;
        const uint32_t t0 = millis();
        if (!command(CMD_SYNC, timeoutMs)) return false;
        while (g_saveReq) {
            if ((uint32_t)(millis() - t0) >= timeoutMs) return false;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        return true;
    }

    Res save(uint32_t* segs)
    {
        if (segs) *segs = 0;
        if (!g_ready) return Res::FAILED;
        if (movingNow()) return Res::MOVING;
        if (!command(CMD_SAVE, 1000)) return Res::BUSY;
        if (segs) { Lock l; *segs = countLocked(SEG_SEALED) + (g_sp.seg >= 0 ? 1 : 0); }
        LOGI("cap", "save (web): %u segment(s) to flash", segs ? (unsigned)*segs : 0u);
        return Res::OK;
    }

    bool takeSegment(uint8_t* out, size_t cap, SegInfo& info)
    {
        if (!g_ready || !out) return false;
        Lock l;
        const int i = oldestLocked(SEG_SEALED);
        if (i < 0 || g_segs[i].len > cap) return false;
        Seg& s = g_segs[i];
        memcpy(out, s.buf, s.len);
        s.state = SEG_SENDING;
        info.seq = s.seq; info.session = s.session; info.len = s.len;
        info.records = s.records; info.part = s.part;
        return true;
    }

    void segmentDone(uint32_t seq, bool sent)
    {
        if (!g_ready) return;
        Lock l;
        for (uint16_t i = 0; i < g_nSegs; ++i) {
            Seg& s = g_segs[i];
            if (s.state != SEG_SENDING || s.seq != seq) continue;   // taken back meanwhile
            s.state = sent ? SEG_FREE : SEG_SEALED;
            if (sent) ++g_sent;
            return;
        }
    }

    uint32_t unsentSegments()
    {
        if (!g_ready) return 0;
        Lock l;
        return countLocked(SEG_SEALED);
    }

    Status status()
    {
        Status s = {};
        s.ready = g_ready;
        s.recording = g_rec;
        s.mode = g_mode;
        s.frames = g_frames; s.written = g_written; s.dropped = g_dropped; s.markers = g_markers;
        s.ringUsed = g_count; s.ringCap = g_cap;
        s.session = g_session;
        s.rateFps = g_rateFps;
        s.bitrate[0] = g_bitrate[0]; s.bitrate[1] = g_bitrate[1];
        s.ids = g_filter.ids();
        s.filterOverflow = g_filter.overflow();
        s.lastError = (const char*)g_err;
        s.segs = g_nSegs;
        s.segBytes = (uint32_t)g_segBytes;
        s.sealed = g_sealed; s.sent = g_sent; s.spilled = g_spilled;
        s.lostSegs = g_lostSegs; s.lostRecs = g_lostRecs;
        s.spillMaxUs = g_spillMaxUs;
        s.spill = g_set.spill;
        s.bytesPerRec100 = g_sealedRecs ? (uint32_t)((uint64_t)g_sealedBytes * 100 / g_sealedRecs) : 0;
        s.standing = standingNow();
        s.moving = movingNow();
        if (!g_ready) return s;
        s.budget = budgetBytes();
        // Walks the filesystem: the web asks, capwr never does.
        const size_t total = g_fsTotal, used = LittleFS.usedBytes();
        s.fsFree = total > used ? (uint32_t)(total - used) : 0;
        Lock l;
        s.capBytes = capBytesLocked();
        s.files = g_nFiles;
        uint16_t n = 0;
        s.unsentBytes = unsentLocked(&n);
        s.unsentSegs = n;
        if (g_open >= 0) {
            s.curFile = g_segs[g_open].seq;
            s.curBytes = (uint32_t)g_enc.size();
            s.unsentBytes += s.curBytes;
        }
        s.saving = g_saveReq || g_sp.seg >= 0;
        return s;
    }

    size_t list(FileInfo* out, size_t max)
    {
        if (!g_mx) return 0;
        Lock l;
        for (uint16_t i = 0; i < g_nFiles && i < max; ++i) {
            const FileRec& f = g_files[i];
            out[i].num = f.num; out[i].session = f.session;
            out[i].size = f.size; out[i].wallSec = f.wallSec;
            out[i].part = f.part; out[i].prot = f.prot;
            out[i].current = false;   // files are whole segments or closed v1 files
        }
        return g_nFiles;
    }

    Res remove(uint32_t num)
    {
        if (!g_ready) return Res::FAILED;
        Lock l;
        const int i = indexOf(num);
        if (i < 0) return Res::NOT_FOUND;
        if (!deletable(g_files[i])) return Res::BUSY;
        if (!deleteLocked((uint16_t)i)) return Res::FAILED;
        LOGI("cap", "deleted %04lu (web)", (unsigned long)num);
        return Res::OK;
    }

    uint32_t removeUnprotected()
    {
        if (!g_ready) return 0;
        uint32_t n = 0;
        Lock l;
        for (int i = (int)g_nFiles - 1; i >= 0; --i)
            if (!g_files[i].prot && deletable(g_files[i]) && deleteLocked((uint16_t)i)) ++n;
        LOGI("cap", "deleted %lu unprotected file(s) (web)", (unsigned long)n);
        return n;
    }

    Res protect(uint32_t num, bool on)
    {
        if (!g_ready) return Res::FAILED;
        Lock l;
        const int i = indexOf(num);
        if (i < 0) return Res::NOT_FOUND;
        const uint32_t session = g_files[i].session;
        setProtLocked(session, on);
        // The live session: new parts follow, and a later marker re-protects.
        if (session == g_session) g_sessionProt = on;
        LOGI("cap", "session %04lu %s (web)", (unsigned long)session, on ? "protected" : "unprotected");
        return Res::OK;
    }

    uint32_t formatArea()
    {
        if (!g_ready) return 0;
        g_formatCount = 0;
        command(CMD_FORMAT, 5000);
        return g_formatCount;
    }

    Res beginRead(uint32_t num, char* path, size_t n)
    {
        if (!g_ready) return Res::FAILED;
        Lock l;
        const int i = indexOf(num);
        if (i < 0) return Res::NOT_FOUND;
        ++g_files[i].readers;
        binPath(num, path, n);
        return Res::OK;
    }

    void endRead(uint32_t num)
    {
        if (!g_mx) return;
        Lock l;
        const int i = indexOf(num);
        if (i >= 0 && g_files[i].readers) --g_files[i].readers;
    }
}
#endif
