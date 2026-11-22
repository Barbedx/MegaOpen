#include "LogQ.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_attr.h>
#include <esp_timer.h>

// The ring is plain DRAM (.bss) and post() is IRAM: an ISR with the cache off
// can still post. The lock is held for one 28-byte copy.
namespace LogQ
{
    namespace
    {
        DRAM_ATTR Rec              s_ring[SLOTS];
        DRAM_ATTR uint32_t         s_head = 0;    // next slot to write
        DRAM_ATTR uint32_t         s_tail = 0;    // next slot to take
        DRAM_ATTR uint32_t         s_posted = 0;
        DRAM_ATTR uint32_t         s_dropped = 0;
        DRAM_ATTR TaskHandle_t volatile s_wake = nullptr;
        DRAM_ATTR portMUX_TYPE     s_mux = portMUX_INITIALIZER_UNLOCKED;
    }

    void IRAM_ATTR post(const char* cat, const char* fmt, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
    {
        const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool ok;
        portENTER_CRITICAL_SAFE(&s_mux);
        ok = s_head - s_tail < (uint32_t)SLOTS;
        if (ok) {
            Rec& r = s_ring[s_head % SLOTS];
            r.cat = cat;
            r.fmt = fmt;
            r.a[0] = a0; r.a[1] = a1; r.a[2] = a2; r.a[3] = a3;
            r.ms = ms;
            ++s_head;
            ++s_posted;
        } else {
            ++s_dropped;
        }
        portEXIT_CRITICAL_SAFE(&s_mux);
        const TaskHandle_t t = s_wake;
        if (!ok || !t) return;
        if (xPortInIsrContext()) {
            BaseType_t woke = pdFALSE;
            vTaskNotifyGiveFromISR(t, &woke);
            if (woke) portYIELD_FROM_ISR();
        } else {
            xTaskNotifyGive(t);
        }
    }

    void setWake(void* task) { s_wake = (TaskHandle_t)task; }

    bool take(Rec& r)
    {
        bool ok;
        portENTER_CRITICAL(&s_mux);
        ok = s_head != s_tail;
        if (ok) { r = s_ring[s_tail % SLOTS]; ++s_tail; }
        portEXIT_CRITICAL(&s_mux);
        return ok;
    }

    uint32_t posted()  { return s_posted; }
    uint32_t dropped() { return s_dropped; }
}
