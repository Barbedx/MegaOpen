#include "Rt.h"
#include "../utils/LogBus.h"
#include "../Features.h"
#if FEATURE_TASK_WDT
#include <esp_task_wdt.h>
#endif

namespace App
{
    TaskHandle_t spawn(const TaskSpec& s)
    {
        const int id = Tasks::add(s.name, s.critical);
        if (s.idOut) *s.idOut = id;
        if (id < 0)
            LOGI("task", "registry full: %s runs unlisted", s.name);

        TaskHandle_t h = nullptr;
        const BaseType_t ok = xTaskCreatePinnedToCore(s.fn, s.name, s.stack, s.arg,
                                                      s.prio, &h, core(s.core));
        if (ok != pdPASS) {
            // The entry stays, critical or not. A critical task that never
            // starts never beats, so the OTA trial fails on purpose: an image
            // that runs out of heap for its knobs is not healthy. This line is
            // the reason; the verdict line later only says the task is late.
            LOGI("task", "%s FAILED to start (stack %u, free heap %u)%s", s.name,
                           (unsigned)s.stack, (unsigned)ESP.getFreeHeap(),
                           s.critical ? ": critical, so this image fails its OTA trial" : "");
            return nullptr;
        }
        Tasks::setHandle(id, h);
#if FEATURE_TASK_WDT
        // Fed by Tasks::beat on every pass. Off by default: 1.12 only logs
        // (Supervisor.h); env:t2can_wdt builds this path.
        const uint32_t lim = Plan::lateLimitMs(s.name);
        if (s.wdt || (lim && lim <= Plan::WDT_LATE_LIMIT_MAX)) {
            const esp_err_t e = esp_task_wdt_add(h);
            LOGI("task", "%s: task watchdog %s", s.name, e == ESP_OK ? "on" : "FAILED");
        }
#endif
        return h;
    }
}
