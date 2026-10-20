#include "LogFile.h"
#if FEATURE_LOGFS
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include "LogBus.h"

namespace LogFile
{
    namespace
    {
        bool     g_mounted = false;
        File     g_cur;
        size_t   g_curSize = 0;       // bytes of /log/0.log on flash
        char     g_path[16];
        const char* g_err = "";

        // The RAM side: lines wait here until a flush. PSRAM when the board
        // has it (the T-2CAN does), else internal; with neither, write-through.
        char*    g_buf = nullptr;
        size_t   g_bufCap = 0;
        size_t   g_bufLen = 0;
        volatile bool g_flushReq = false;

        const char* nameOf(int n)
        {
            snprintf(g_path, sizeof(g_path), "/log/%d.log", n);
            return g_path;
        }

        // Oldest deleted, n -> n+1, a fresh 0 opened.
        void rotate()
        {
            if (g_cur) g_cur.close();
            char from[16], to[16];
            snprintf(to, sizeof(to), "/log/%d.log", FILES - 1);
            if (LittleFS.exists(to)) LittleFS.remove(to);
            for (int i = FILES - 2; i >= 0; --i) {
                snprintf(from, sizeof(from), "/log/%d.log", i);
                snprintf(to, sizeof(to), "/log/%d.log", i + 1);
                if (LittleFS.exists(from)) LittleFS.rename(from, to);
            }
            g_cur = LittleFS.open("/log/0.log", "w");
            g_curSize = 0;
        }

        void writeOut(const uint8_t* p, size_t n)
        {
            if (!n || !g_cur) return;
            g_cur.write(p, n);
            g_curSize += n;
        }

        // Log::emit's "<L> [cat] ..." with L = E or W: the event trigger.
        bool isWarning(const char* line, int len)
        {
            return len >= 3 && (line[0] == 'E' || line[0] == 'W') && line[1] == ' ' && line[2] == '[';
        }

        void allocBuf()
        {
            if (g_buf) return;
            if (psramFound())
                g_buf = (char*)heap_caps_malloc(FLUSH_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!g_buf)
                g_buf = (char*)heap_caps_malloc(FLUSH_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            g_bufCap = g_buf ? FLUSH_BYTES : 0;
        }
    }

    bool begin()
    {
        // The table names the partition "littlefs"; the core's default is "spiffs".
        // formatOnFail=false: a failed mount after a power cut must not wipe the
        // very logs that would explain it. Formatted here only the first time.
        //
        // The partition's size is the other exception: a USB flash with a new
        // table (the T-2CAN re-split, 2026-10-15: littlefs 3.9 MB at 0xC10000
        // -> 9.9 MB at 0x610000) leaves no superblock that fits it, so the
        // mount fails, and keeping that partition "safe" would only leave the
        // log off for good. A size we did not mount last time (or none stored:
        // a board from before the size was kept) formats ONCE; the size is
        // stored on the next good mount, and later failures are kept again.
        const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                               ESP_PARTITION_SUBTYPE_ANY, "littlefs");
        const uint32_t partSize = part ? (uint32_t)part->size : 0;
        Preferences p;
        p.begin("sys", false);
        const bool seen = p.getBool("logfsInit", false);
        const uint32_t lastSize = p.getUInt("logfsSize", 0);
        const bool resized = seen && partSize && lastSize != partSize;
        g_mounted = LittleFS.begin(false, "/littlefs", 4, "littlefs");
        bool formatted = false;
        if (!g_mounted && (!seen || resized)) {
            // Never mounted on this board, or a new table: nothing to lose.
            formatted = true;
            g_mounted = LittleFS.format() && LittleFS.begin(false, "/littlefs", 4, "littlefs");
        }
        if (g_mounted && !seen) p.putBool("logfsInit", true);
        if (g_mounted && partSize && lastSize != partSize) p.putUInt("logfsSize", partSize);
        p.end();
        if (!g_mounted) {
            g_err = seen && !resized ? "mount failed; partition kept, not formatted (POST /api/logs/format erases it)"
                  : resized          ? "partition resized; format failed"
                                     : "first-time format failed";
            return false;
        }
        g_err = "";
        // Queued: LogBus holds lines until its task drains them.
        if (formatted && resized)
            LogBus::printf("[log] littlefs reformatted: its partition is now %lu kB (was %lu kB); "
                           "older logs and captures are gone", (unsigned long)(partSize / 1024),
                           (unsigned long)(lastSize / 1024));
        if (!LittleFS.exists("/log")) LittleFS.mkdir("/log");
        allocBuf();
        rotate();
        return (bool)g_cur;
    }

    bool mounted() { return g_mounted && (bool)g_cur; }
    bool fsMounted() { return g_mounted; }
    const char* mountError() { return g_err; }

    bool format()
    {
        if (g_mounted) return false;
        if (!LittleFS.format()) return false;
        // The LogBus file sink was not added this boot; the next boot logs.
        g_mounted = LittleFS.begin(false, "/littlefs", 4, "littlefs");
        if (g_mounted) {
            LittleFS.mkdir("/log");
            const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                   ESP_PARTITION_SUBTYPE_ANY, "littlefs");
            Preferences p;
            p.begin("sys", false);
            p.putBool("logfsInit", true);
            if (part) p.putUInt("logfsSize", (uint32_t)part->size);
            p.end();
            g_err = "";
        }
        return g_mounted;
    }

    void flushNow()
    {
        if (!g_mounted || !g_cur) return;
        writeOut((const uint8_t*)g_buf, g_bufLen);
        g_bufLen = 0;
        g_cur.flush();
        if (g_curSize >= FILE_MAX) rotate();
    }

    void requestFlush() { g_flushReq = true; }

    void sink(const char* line, int len, uint32_t ms)
    {
        if (!g_mounted || !g_cur) return;
        char stamp[16];
        const int sn = snprintf(stamp, sizeof(stamp), "%lu.%03lu ",
                                (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
        const size_t need = (size_t)sn + (size_t)len + 1;

        if (!g_buf) {
            // No RAM for a buffer: straight through (the filesystem's own
            // cache still batches it), synced on events only.
            writeOut((const uint8_t*)stamp, sn);
            writeOut((const uint8_t*)line, len);
            writeOut((const uint8_t*)"\n", 1);
        } else {
            if (g_bufLen + need > g_bufCap) flushNow();   // full: the size trigger
            if (need <= g_bufCap) {
                memcpy(g_buf + g_bufLen, stamp, sn);
                memcpy(g_buf + g_bufLen + sn, line, len);
                g_buf[g_bufLen + sn + len] = '\n';
                g_bufLen += need;
            }
        }

        if (isWarning(line, len) || g_flushReq) {
            g_flushReq = false;
            flushNow();
        } else if (!g_buf && g_curSize >= FILE_MAX) {
            g_cur.flush();
            rotate();
        }
    }

    const char* path(int n)
    {
        if (!g_mounted || n < 0 || n >= FILES) return nullptr;
        const char* p = nameOf(n);
        return LittleFS.exists(p) ? p : nullptr;
    }

    size_t size(int n)
    {
        const char* p = path(n);
        if (!p) return 0;
        if (n == 0) return g_curSize;   // the open file's size is ours, not the FS's
        File f = LittleFS.open(p, "r");
        const size_t s = f ? f.size() : 0;
        if (f) f.close();
        return s;
    }

    size_t buffered()   { return g_bufLen; }
    size_t totalBytes() { return g_mounted ? LittleFS.totalBytes() : 0; }
    size_t usedBytes()  { return g_mounted ? LittleFS.usedBytes() : 0; }
}
#endif
