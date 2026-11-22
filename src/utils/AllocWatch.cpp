#include "AllocWatch.h"
#include <string.h>
#include <esp_heap_caps.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Log.h"

namespace AllocWatch
{
    namespace
    {
        portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
        Snap         s_last;           // under s_mux
        uint32_t     s_said = 0;       // the count the last report() line had (net_svc only)
        uint32_t     s_saidAt = 0;

        // ctx: any task or ISR, inside the failing malloc. Counts only.
        void onFail(size_t size, uint32_t caps, const char* /*fn*/)
        {
            const bool isr = xPortInIsrContext();
            const char* name = isr ? "isr" : pcTaskGetName(nullptr);
            const uint32_t at = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
            portENTER_CRITICAL_SAFE(&s_mux);
            ++s_last.count;
            s_last.size = (uint32_t)size;
            s_last.caps = caps;
            s_last.atMs = at;
            size_t i = 0;
            if (name)
                for (; i + 1 < sizeof(s_last.task) && name[i]; ++i) s_last.task[i] = name[i];
            s_last.task[i] = 0;
            portEXIT_CRITICAL_SAFE(&s_mux);
        }
    }

    void begin() { heap_caps_register_failed_alloc_callback(onFail); }

    void get(Snap& out)
    {
        portENTER_CRITICAL(&s_mux);
        out = s_last;
        portEXIT_CRITICAL(&s_mux);
    }

    void report(uint32_t now)
    {
        if (s_last.count == s_said) return;   // a word read: a torn value only delays the line
        if (s_said && now - s_saidAt < REPORT_MS) return;
        Snap s;
        get(s);
        LOGI("mem", "alloc failed n=%u (+%u) last size=%u caps=0x%x task=%s at=%u",
             (unsigned)s.count, (unsigned)(s.count - s_said), (unsigned)s.size, (unsigned)s.caps,
             s.task, (unsigned)s.atMs);
        s_said = s.count;
        s_saidAt = now;
    }
}
