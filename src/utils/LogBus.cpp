#include "LogBus.h"
#include "LogRing.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <esp_heap_caps.h>

// Ring size from the build: 4 kB fits the DevKit's heap budget; the T-2CAN has
// RAM to hold a burst of seconds. The same bytes keep the lines already sent
// for the web log (LogRing.h), so it costs no second buffer.
#ifndef LOGBUS_RING
#define LOGBUS_RING 4096
#endif

// A board with PSRAM keeps the ring there (32 kB of internal RAM back on the
// T-2CAN). Until setup() calls grow(), lines go to a small internal boot ring;
// grow() moves them into the big one. With no PSRAM found, the ring is
// LOGBUS_RING_INTERNAL bytes of internal heap. Without BOARD_HAS_PSRAM the ring
// is static, as before.
#if defined(BOARD_HAS_PSRAM)
#define LOGBUS_GROWS 1
#ifndef LOGBUS_BOOT_RING
#define LOGBUS_BOOT_RING 1024
#endif
#ifndef LOGBUS_RING_INTERNAL
#define LOGBUS_RING_INTERNAL 4096
#endif
#else
#define LOGBUS_GROWS 0
#define LOGBUS_BOOT_RING LOGBUS_RING
#endif

namespace LogBus
{
    namespace
    {
        uint8_t      g_buf[LOGBUS_BOOT_RING];
        LogRing      g_ring(g_buf, LOGBUS_BOOT_RING);
        uint32_t     g_size = LOGBUS_BOOT_RING;
        bool         g_inPsram = false;
        portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
        uint32_t     g_dropped = 0;
        Sink         g_sinks[4] = {};
        uint8_t      g_nSinks = 0;
        TaskHandle_t g_task = nullptr;
        volatile WakeHook g_wake = nullptr;
        uint32_t     g_droppedSaid = 0;   // the drop count last written to the sinks

        // flushAndWait(): asked by any task, done by this one.
        void (*volatile g_onFlush)() = nullptr;
        volatile bool     g_flushReq = false;
        SemaphoreHandle_t g_flushDone = nullptr;

        // One line out of the ring into buf; its length, -1 when none waits.
        int take(char* buf, uint32_t& ms)
        {
            portENTER_CRITICAL(&g_mux);
            const int n = g_ring.take(buf, ms);
            portEXIT_CRITICAL(&g_mux);
            return n;
        }

        void fanOut(const char* b, int n, uint32_t ms)
        {
            for (uint8_t i = 0; i < g_nSinks; ++i) g_sinks[i](b, n, ms);
        }

        // Lines the ring refused since the last notice, as a line of their own,
        // so the flash log shows where it has a gap.
        void sayDropped()
        {
            const uint32_t d = g_dropped;
            if (d == g_droppedSaid) return;
            char b[64];
            const int n = snprintf(b, sizeof(b), "[log] %u line(s) dropped (ring full)",
                                   (unsigned)(d - g_droppedSaid));
            g_droppedSaid = d;
            fanOut(b, n, millis());
        }

        void drainAll(char* buf)
        {
            for (;;) {
                uint32_t ms = 0;
                const int n = take(buf, ms);
                if (n < 0) break;
                sayDropped();
                fanOut(buf, n, ms);
            }
            sayDropped();
        }

        void taskBody(void*)
        {
            char buf[MAX_LINE + 1];
            for (;;) {
                // Woken by line(); the timeout only bounds a missed notify.
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
                const uint32_t wokeUs = micros();
                drainAll(buf);
                if (g_flushReq) {
                    g_flushReq = false;
                    void (*f)() = g_onFlush;
                    if (f) f();
                    if (g_flushDone) xSemaphoreGive(g_flushDone);
                }
                const WakeHook h = g_wake;
                if (h) h(micros() - wokeUs);
            }
        }
    }

    bool grow()
    {
#if LOGBUS_GROWS
        static bool s_grown = false;
        if (s_grown) return true;
        uint32_t size = LOGBUS_RING;
        void* m = psramFound() ? heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
        const bool ps = m != nullptr;
        if (!m) {
            size = LOGBUS_RING_INTERNAL;
            m = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!m) return false;
        portENTER_CRITICAL(&g_mux);
        const bool ok = g_ring.moveTo((uint8_t*)m, size);
        if (ok) { g_size = size; g_inPsram = ps; }
        portEXIT_CRITICAL(&g_mux);
        if (!ok) heap_caps_free(m);
        s_grown = ok;
        return ok;
#else
        return true;
#endif
    }

    bool ringInPsram() { return g_inPsram; }

    void addSink(Sink s)
    {
        if (g_nSinks < 4) g_sinks[g_nSinks++] = s;
    }

    void line(const char* s)
    {
        int n = (int)strnlen(s, MAX_LINE);
        while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) --n;
        const uint32_t ms = millis();
        portENTER_CRITICAL(&g_mux);
        const bool ok = g_ring.push(s, (uint16_t)n, ms);
        if (!ok) ++g_dropped;
        portEXIT_CRITICAL(&g_mux);
        if (ok && g_task) xTaskNotifyGive(g_task);
    }

    void printf(const char* fmt, ...)
    {
        char b[MAX_LINE];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        line(b);
    }

    void startTask(UBaseType_t prio, BaseType_t core)
    {
        if (g_task) return;
        if (!g_flushDone) g_flushDone = xSemaphoreCreateBinary();
        // 4 kB: the sinks format a timestamp and may call into LittleFS.
        xTaskCreatePinnedToCore(taskBody, "logbus", 4096, nullptr, prio, &g_task, core);
    }

    void onFlush(void (*fn)()) { g_onFlush = fn; }

    bool flushAndWait(uint32_t timeoutMs)
    {
        if (!g_task || !g_flushDone) return false;
        if (xTaskGetCurrentTaskHandle() == g_task) {
            // Already the LogBus task (a sink that rebooted): do it here.
            char buf[MAX_LINE + 1];
            drainAll(buf);
            void (*f)() = g_onFlush;
            if (f) f();
            return true;
        }
        xSemaphoreTake(g_flushDone, 0);   // a stale give from an earlier timeout
        g_flushReq = true;
        xTaskNotifyGive(g_task);
        return xSemaphoreTake(g_flushDone, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
    }

    TaskHandle_t task()       { return g_task; }
    void onWake(WakeHook h)   { g_wake = h; }

    uint32_t dropped() { return g_dropped; }
    uint32_t lines()   { return g_ring.last(); }
    int      ringSize() { return (int)g_size; }

    Cursor after(uint32_t since)
    {
        // A number past the newest line is a page from before a reboot: from the start.
        if (since > g_ring.last()) since = 0;
        return Cursor{since + 1, 0, 0};
    }

    int read(Cursor& c, char* buf, uint32_t& ms, uint32_t& lost)
    {
        // The lock is held for one line's copy, or a walk of 32 headers: line()
        // on another core waits no longer than it does for the LogBus task.
        int n;
        do {
            portENTER_CRITICAL(&g_mux);
            n = g_ring.read(c, buf, ms, lost, 32);
            portEXIT_CRITICAL(&g_mux);
        } while (n == -2);
        return n;
    }
}
