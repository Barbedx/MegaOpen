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

namespace Capture
{
    namespace
    {
        using namespace CapFormat;

        // ---- sizes ----------------------------------------------------------------
        constexpr size_t   RING_BYTES_PSRAM = 1024 * 1024;   // ~13 s of a full 500k bus
        constexpr size_t   RING_BYTES_SMALL = 32 * 1024;
        constexpr size_t   CHUNK            = 16 * 1024;     // one flash write
        constexpr uint16_t IDS_PSRAM        = 1024;          // change-only table
        constexpr uint16_t IDS_SMALL        = 128;
        constexpr uint16_t MAX_FILES        = 128;
        // Room kept free beyond the budget for littlefs's own metadata and the
        // log's open block: the budget is a promise, not a hope.
        constexpr size_t   FS_SLACK         = 256 * 1024;
        constexpr uint32_t WAKE_MS          = 50;
        constexpr uint32_t LOG_MS           = 10000;
        constexpr uint32_t DRAIN_BATCH      = 4096;          // slots per pass, then beat

        // capwr: below everything, on the WiFi core (the plan's numbers; not in
        // src/app/TaskPlan.h yet). 6144: LittleFS calls and the [cap] printf.
        constexpr UBaseType_t CAPWR_PRIO  = 1;
        constexpr int         CAPWR_CORE  = 0;
        constexpr uint32_t    CAPWR_STACK = 6144;

        // A ring slot: the frame as pushed. 20 bytes with alignment.
        struct Slot {
            uint32_t t;
            uint32_t w;
            uint8_t  len;
            uint8_t  kind;
            uint8_t  data[8];
        };

        // ---- the ring (producers + capwr) ------------------------------------------
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
        enum : uint32_t { CMD_START = 1, CMD_STOP = 2, CMD_SYNC = 4, CMD_FORMAT = 8, CMD_MODE = 16 };
        volatile uint32_t g_cmdBits = 0;
        volatile uint32_t g_reqSeq = 0, g_doneSeq = 0;
        volatile uint8_t  g_modeReq = MODE_CHANGE;
        volatile uint32_t g_formatCount = 0;
        TaskHandle_t      g_task = nullptr;
        int               g_taskId = -1;

        // ---- the writer (capwr only, unless noted) -----------------------------------
        uint8_t*     g_chunk = nullptr;
        size_t       g_chunkLen = 0;
        File         g_file;
        volatile uint32_t g_curNum = 0;         // 0 = none open; written under g_mx
        volatile uint32_t g_curBytes = 0;       // on flash
        uint32_t     g_session = 0;             // 0 = session pending its first file
        uint16_t     g_part = 0;
        bool         g_active = false;          // a session is running
        bool         g_sessionProt = false;
        bool         g_markPending = false;     // a marker is in the chunk: write it out
        volatile uint8_t g_mode = MODE_CHANGE;
        ChangeFilter g_filter;
        ChangeFilter::Entry* g_table = nullptr;
        volatile uint32_t g_written = 0;        // records
        volatile uint32_t g_rateFps = 0;
        bool         g_noRoomLogged = false;

        // ---- the file table (g_mx) ------------------------------------------------------
        struct FileRec {
            uint32_t num, session, size, wallSec;
            uint16_t part;
            bool     prot;
            uint8_t  readers;
        };
        SemaphoreHandle_t g_mx = nullptr;
        FileRec   g_files[MAX_FILES];
        uint16_t  g_nFiles = 0;
        uint32_t  g_next = 1;
        Settings  g_set;
        const char* volatile g_err = "";
        bool      g_ready = false;

        struct Lock {
            Lock()  { xSemaphoreTake(g_mx, portMAX_DELAY); }
            ~Lock() { xSemaphoreGive(g_mx); }
        };

        void binPath(uint32_t n, char* p, size_t cap)  { snprintf(p, cap, "/cap/%04lu.bin", (unsigned long)n); }
        void keepPath(uint32_t n, char* p, size_t cap) { snprintf(p, cap, "/cap/%04lu.keep", (unsigned long)n); }

        // ---- settings ------------------------------------------------------------------
        Settings clampSettings(Settings s)
        {
            if (s.fillPct < 10) s.fillPct = 10;
            if (s.fillPct > 95) s.fillPct = 95;
            if (s.fileMB < 1) s.fileMB = 1;
            if (s.fileMB > 8) s.fileMB = 8;
            if (s.mode > MODE_FULL) s.mode = MODE_CHANGE;
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
            g_next        = p.getUInt("next", 1);
            p.end();
            g_set = clampSettings(s);
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
            if (g_nFiles >= MAX_FILES) return;
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

        bool deletable(const FileRec& f) { return f.num != g_curNum && f.readers == 0; }

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

        // Frees space for `need` more bytes. False: nothing may be deleted.
        bool makeRoom(uint32_t need)
        {
            const uint32_t budget = budgetBytes();
            Lock l;
            for (;;) {
                if (capBytesLocked() + need <= budget) return true;
                if (!g_set.ring) { g_err = "capture area full and ring is off"; return false; }
                const int v = victimLocked(true);
                if (v < 0) { g_err = "capture area full: nothing left to delete"; return false; }
                const FileRec f = g_files[v];
                if (!deleteLocked((uint16_t)v)) { g_err = "delete failed"; return false; }
                if (f.prot)
                    LogBus::printf("[cap] deleted PROTECTED %04lu (session %04lu): nothing unprotected left",
                                   (unsigned long)f.num, (unsigned long)f.session);
                else
                    LogBus::printf("[cap] ring: deleted %04lu (%lu kB)", (unsigned long)f.num,
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
                uint8_t hb[HEADER_SIZE];
                Header h;
                const size_t got = e.read(hb, sizeof(hb));
                e.close();
                if (f.size <= HEADER_SIZE || !decodeHeader(hb, got, h)) {
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
            if (emptied) LogBus::printf("[cap] removed %u empty capture file(s)", (unsigned)emptied);
        }

        // ---- the writer ---------------------------------------------------------------
        void saveNext()
        {
            Preferences p;
            p.begin("cap", false);
            p.putUInt("next", g_next);
            p.end();
        }

        uint32_t wallNow(uint32_t& atMs)
        {
            atMs = millis();
            const time_t t = time(nullptr);
            return t >= 1700000000 ? (uint32_t)t : 0;
        }

        // Opens the next file and puts its header at the head of the chunk.
        bool openFile()
        {
            {
                Lock l;
                while (g_nFiles >= MAX_FILES) {
                    const int v = victimLocked(true);
                    if (v < 0) { g_err = "file table full"; return false; }
                    const FileRec f = g_files[v];
                    if (!deleteLocked((uint16_t)v)) { g_err = "file table full"; return false; }
                    LogBus::printf("[cap] %u files: deleted %s%04lu", (unsigned)MAX_FILES,
                                   f.prot ? "PROTECTED " : "", (unsigned long)f.num);
                }
            }
            const uint32_t num = g_next++;
            saveNext();
            if (!g_session) { g_session = num; g_part = 0; }
            else ++g_part;

            char p[24];
            binPath(num, p, sizeof(p));
            g_file = LittleFS.open(p, "w");
            if (!g_file) { g_err = "cannot create a capture file"; return false; }

            Header h;
            h.session = g_session;
            h.part = g_part;
            h.mode = g_mode;
            h.bitrate[0] = g_bitrate[0];
            h.bitrate[1] = g_bitrate[1];
            {
                Preferences s;
                s.begin("sys", true);
                h.boot = s.getUInt("boots", 0);
                s.end();
            }
            h.wallSec = wallNow(h.wallAtMs);
            strncpy(h.build, MegaOpen::buildId(), sizeof(h.build) - 1);
            encodeHeader(h, g_chunk);
            g_chunkLen = HEADER_SIZE;
            g_curBytes = 0;
            // Each file starts with every id written, so it decodes on its own.
            g_filter.rekey();

            FileRec f = {};
            f.num = num; f.session = g_session; f.part = g_part; f.wallSec = h.wallSec;
            {
                Lock l;
                insertLocked(f);
                g_curNum = num;
                if (g_sessionProt) setProtLocked(g_session, true);
            }
            LogBus::printf("[cap] file %04lu opened (session %04lu part %u)", (unsigned long)num,
                           (unsigned long)g_session, (unsigned)g_part);
            return true;
        }

        // The chunk to flash: one write and one sync.
        void writeChunk()
        {
            if (!g_chunkLen || !g_file) return;
            if (!makeRoom((uint32_t)g_chunkLen)) {
                if (!g_noRoomLogged) {
                    LogBus::printf("[cap] chunk dropped: %s", (const char*)g_err);
                    g_noRoomLogged = true;
                }
                // The header stays at the front of an empty file.
                g_chunkLen = g_curBytes ? 0 : HEADER_SIZE;
                return;
            }
            g_noRoomLogged = false;
            const size_t w = g_file.write(g_chunk, g_chunkLen);
            g_file.flush();
            g_curBytes += (uint32_t)w;
            {
                Lock l;
                const int i = indexOf(g_curNum);
                if (i >= 0) g_files[i].size = g_curBytes;
            }
            if (w != g_chunkLen) g_err = "short write (filesystem full?)";
            g_chunkLen = 0;
        }

        void closeFile()
        {
            if (!g_file) return;
            writeChunk();
            g_file.close();
            const uint32_t num = g_curNum;
            Lock l;
            g_curNum = 0;
            const int i = indexOf(num);
            // Nothing but the header reached flash: not a capture.
            if (i >= 0 && g_files[i].size <= HEADER_SIZE) deleteLocked((uint16_t)i);
        }

        void append(const Rec& r)
        {
            if (!g_file && !openFile()) return;
            const uint32_t fileMax = (uint32_t)g_set.fileMB * 1024u * 1024u;
            if (g_curBytes + g_chunkLen + REC_SIZE > fileMax) {
                closeFile();
                if (!openFile()) return;
            }
            if (g_chunkLen + REC_SIZE > CHUNK) writeChunk();
            encodeRec(r, g_chunk + g_chunkLen);
            g_chunkLen += REC_SIZE;
            ++g_written;
        }

        void statsOut(const Rec& r, void*) { append(r); }

        void beginSession()
        {
            g_session = 0;
            g_part = 0;
            g_sessionProt = false;
            g_markPending = false;
            g_active = true;
            g_filter.reset();
            g_filter.startStats(millis());
            g_rec = true;
        }

        void endSession()
        {
            closeFile();
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

        // Up to DRAIN_BATCH slots; true when the ring still holds more.
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
            LogBus::printf("[cap] session %04lu protected (marker)", (unsigned long)g_session);
        }

        uint32_t formatLocked()
        {
            uint32_t n = 0;
            for (int i = (int)g_nFiles - 1; i >= 0; --i)
                if (deletable(g_files[i]) && deleteLocked((uint16_t)i)) ++n;
            return n;
        }

        void logLine()
        {
            char b[200];
            statusLine(b, sizeof(b));
            LogBus::line(b);
        }

        void run(void*)
        {
            uint32_t logAt = millis();
            uint32_t framesAt = g_frames;
            for (;;) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WAKE_MS));
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
                        LogBus::line("[cap] stopped");
                    }
                }
                if (cmd & CMD_FORMAT) {
                    const bool was = g_active;
                    g_rec = false;
                    drainAll();
                    endSession();
                    uint32_t n;
                    { Lock l; n = formatLocked(); }
                    g_formatCount = n;
                    g_err = "";
                    LogBus::printf("[cap] capture area formatted: %lu file(s) deleted", (unsigned long)n);
                    if (was) beginSession();
                }
                if ((cmd & CMD_START) && !g_active) {
                    beginSession();
                    LogBus::printf("[cap] recording (%s)", g_mode == MODE_FULL ? "full" : "change-only");
                }
                if ((cmd & CMD_MODE) && g_modeReq != g_mode) {
                    drainAll();
                    g_mode = g_modeReq;
                    g_filter.reset();
                    if (g_active && g_file) {
                        Rec r = u32Rec(MODE, millis(), 0, g_mode);
                        r.len = 1;
                        append(r);
                    }
                }

                bool more = false;
                if (g_active || g_count) more = drain();
                if (g_active && g_mode == MODE_CHANGE && g_filter.statsDue(millis()))
                    g_filter.emitStats(millis(), statsOut, nullptr);

                if (g_markPending) {
                    g_markPending = false;
                    writeChunk();   // a marker survives a power cut
                    if (g_set.autoProtect) protectCurrentSession();
                }
                if (cmd & CMD_SYNC) writeChunk();
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
            g_err = "littlefs not mounted";
            LogBus::line("[cap] off: littlefs not mounted");
            return;
        }

        const bool ps = psramFound();
        const uint32_t caps = ps ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t ringBytes = ps ? RING_BYTES_PSRAM : RING_BYTES_SMALL;
        const uint16_t ids = ps ? IDS_PSRAM : IDS_SMALL;
        g_ring  = (Slot*)heap_caps_malloc(ringBytes, caps);
        g_chunk = (uint8_t*)heap_caps_malloc(CHUNK, caps);
        g_table = (ChangeFilter::Entry*)heap_caps_malloc(sizeof(ChangeFilter::Entry) * ids, caps);
        if (!g_ring || !g_chunk || !g_table) {
            heap_caps_free(g_ring); heap_caps_free(g_chunk); heap_caps_free(g_table);
            g_ring = nullptr; g_chunk = nullptr; g_table = nullptr;
            g_err = "no memory for the ring";
            LogBus::line("[cap] off: no memory for the ring");
            return;
        }
        g_cap = (uint32_t)(ringBytes / sizeof(Slot));
        g_fsTotal = LittleFS.totalBytes();
        g_filter.begin(g_table, ids);

        scan();

        App::TaskSpec spec;
        spec.name  = "capwr";
        spec.fn    = run;
        spec.stack = CAPWR_STACK;
        spec.prio  = CAPWR_PRIO;
        spec.core  = CAPWR_CORE;
        spec.idOut = &g_taskId;
        g_task = App::spawn(spec);
        if (!g_task) { g_err = "capwr did not start"; return; }
        g_ready = true;

        uint32_t capB, nF;
        { Lock l; capB = capBytesLocked(); nF = g_nFiles; }
        LogBus::printf("[cap] ready: ring %u kB (%s, %lu slots), %lu file(s) %lu kB of %lu kB budget, next %04lu, %s, autostart %s",
                       (unsigned)(ringBytes / 1024), ps ? "psram" : "internal", (unsigned long)g_cap,
                       (unsigned long)nF, (unsigned long)(capB / 1024), (unsigned long)(budgetBytes() / 1024),
                       (unsigned long)g_next, g_mode == MODE_FULL ? "full" : "change-only",
                       g_set.autostart ? "on" : "off");
        if (g_set.autostart) start();
    }

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
        LogBus::printf("[cap] mark #%u \"%s\"%s file=%04lu", (unsigned)no, t,
                       stored ? "" : (g_rec ? " NOT RECORDED (ring full)" : " (not recording)"),
                       (unsigned long)g_curNum);
    }

    void setBitrate(uint8_t bus, uint32_t bps)
    {
        if (bus > 1 || g_bitrate[bus] == bps) return;
        g_bitrate[bus] = bps;
        if (!g_rec) return;
        // Ordered with the frames: through the ring like them.
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
        uint32_t capB;
        { Lock l; capB = capBytesLocked(); }
        const uint32_t budget = budgetBytes();
        const uint32_t freeK = budget > capB ? (budget - capB) / 1024 : 0;
        const uint32_t ringPct = g_cap ? (uint32_t)((uint64_t)g_count * 100 / g_cap) : 0;
        snprintf(buf, n, "[cap] %s frames=%lu written=%lu dropped=%lu file=%04lu(%lukB) free=%lukB rate=%lufps "
                         "ring=%lu%% ids=%u car=%lu%s%s",
                 g_rec ? (g_mode == MODE_FULL ? "full" : "chg") : "stopped",
                 (unsigned long)g_frames, (unsigned long)g_written, (unsigned long)g_dropped,
                 (unsigned long)g_curNum, (unsigned long)((g_curBytes + g_chunkLen) / 1024),
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
        if (g_mx) { Lock l; g_set = s; } else g_set = s;
        Preferences p;
        p.begin("cap", false);
        p.putBool("ring", s.ring);
        p.putUChar("fill", s.fillPct);
        p.putUChar("fileMB", s.fileMB);
        p.putBool("autoprot", s.autoProtect);
        p.putUChar("mode", s.mode);
        p.putBool("auto", s.autostart);
        p.end();
        setMode(s.mode);
    }

    void start()  { if (g_ready) command(CMD_START, 1000); }
    void stop()
    {
        if (!g_ready) return;
        g_rec = false;   // producers stop now; capwr writes out what came before
        command(CMD_STOP, 2000);
    }
    void setMode(uint8_t mode)
    {
        if (!g_ready || mode > MODE_FULL) return;
        g_modeReq = mode;
        command(CMD_MODE, 1000);
    }
    bool flush(uint32_t timeoutMs) { return g_ready && command(CMD_SYNC, timeoutMs); }

    Status status()
    {
        Status s = {};
        s.ready = g_ready;
        s.recording = g_rec;
        s.mode = g_mode;
        s.frames = g_frames; s.written = g_written; s.dropped = g_dropped; s.markers = g_markers;
        s.ringUsed = g_count; s.ringCap = g_cap;
        s.curFile = g_curNum;
        s.curBytes = g_curBytes + (uint32_t)g_chunkLen;
        s.session = g_session;
        s.rateFps = g_rateFps;
        s.bitrate[0] = g_bitrate[0]; s.bitrate[1] = g_bitrate[1];
        s.ids = g_filter.ids();
        s.filterOverflow = g_filter.overflow();
        s.lastError = (const char*)g_err;
        if (!g_ready) return s;
        s.budget = budgetBytes();
        // Walks the filesystem: the web asks, capwr never does.
        const size_t total = g_fsTotal, used = LittleFS.usedBytes();
        s.fsFree = total > used ? (uint32_t)(total - used) : 0;
        Lock l;
        s.capBytes = capBytesLocked();
        s.files = g_nFiles;
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
            out[i].current = f.num == g_curNum;
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
        LogBus::printf("[cap] deleted %04lu (web)", (unsigned long)num);
        return Res::OK;
    }

    uint32_t removeUnprotected()
    {
        if (!g_ready) return 0;
        uint32_t n = 0;
        Lock l;
        for (int i = (int)g_nFiles - 1; i >= 0; --i)
            if (!g_files[i].prot && deletable(g_files[i]) && deleteLocked((uint16_t)i)) ++n;
        LogBus::printf("[cap] deleted %lu unprotected file(s) (web)", (unsigned long)n);
        return n;
    }

    uint32_t removeOlderDays(uint32_t days, bool force, uint32_t* skipped)
    {
        uint32_t n = 0, skip = 0;
        if (!g_ready) { if (skipped) *skipped = 0; return 0; }
        uint32_t atMs;
        const uint32_t now = wallNow(atMs);
        Lock l;
        for (int i = (int)g_nFiles - 1; i >= 0; --i) {
            const FileRec& f = g_files[i];
            if (!f.wallSec || !now || f.wallSec > now) { ++skip; continue; }
            if (now - f.wallSec < days * 86400u) continue;
            if ((f.prot && !force) || !deletable(f)) continue;
            if (deleteLocked((uint16_t)i)) ++n;
        }
        if (skipped) *skipped = skip;
        LogBus::printf("[cap] deleted %lu file(s) older than %lu day(s) (web; %lu without a clock kept)",
                       (unsigned long)n, (unsigned long)days, (unsigned long)skip);
        return n;
    }

    uint32_t removeOlderSessions(uint32_t sessions, bool force)
    {
        if (!g_ready) return 0;
        uint32_t n = 0;
        Lock l;
        // Walk newest first, counting distinct sessions; the current one counts.
        uint32_t seen = 0, last = 0;
        uint32_t cutoff = 0;   // sessions below this go
        for (int i = (int)g_nFiles - 1; i >= 0; --i) {
            if (g_files[i].session != last) { last = g_files[i].session; ++seen; }
            if (seen > sessions) { cutoff = last + 1; break; }
        }
        if (!cutoff) return 0;
        for (int i = (int)g_nFiles - 1; i >= 0; --i) {
            const FileRec& f = g_files[i];
            if (f.session >= cutoff) continue;
            if ((f.prot && !force) || !deletable(f)) continue;
            if (deleteLocked((uint16_t)i)) ++n;
        }
        LogBus::printf("[cap] deleted %lu file(s) outside the newest %lu session(s) (web)",
                       (unsigned long)n, (unsigned long)sessions);
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
        LogBus::printf("[cap] session %04lu %s (web)", (unsigned long)session, on ? "protected" : "unprotected");
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
        bool live;
        {
            Lock l;
            const int i = indexOf(num);
            if (i < 0) return Res::NOT_FOUND;
            live = num == g_curNum;
        }
        // The live file: the chunk in RAM goes to flash first, so the download
        // has everything up to now. An explicit dump, like /api/logs/file?n=0.
        if (live) flush(1000);
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
