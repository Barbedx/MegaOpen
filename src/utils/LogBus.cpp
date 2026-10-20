#include "LogBus.h"
#include "LogRing.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Ring size from the build: 4 kB fits the DevKit's heap budget; the T-2CAN has
// RAM to hold a burst of seconds. The same bytes keep the lines already sent
// for the web log (LogRing.h), so it costs no second buffer.
#ifndef LOGBUS_RING
#define LOGBUS_RING 4096
#endif

namespace LogBus
{
    namespace
    {
        uint8_t      g_buf[LOGBUS_RING];
        LogRing      g_ring(g_buf, LOGBUS_RING);
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
    int      ringSize() { return LOGBUS_RING; }

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
