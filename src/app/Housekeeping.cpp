#include "Housekeeping.h"
#include "Rt.h"
#include "../utils/LogBus.h"
#include <esp_heap_caps.h>

namespace App
{
    namespace Housekeeping
    {
        namespace
        {
            constexpr uint32_t PASS_MS = 20;
            constexpr uint32_t BEAT_MS = 10000;
            constexpr uint32_t SLOW_MS = 60000;

            Hooks        s_h;
            int          s_id = -1;
            TaskHandle_t s_task = nullptr;

            // Heap low-water mark per heartbeat window, sampled every pass
            // (ESP.getMinFreeHeap() is since boot). DMA free is separate: WiFi
            // buffers live there.
            uint32_t s_heapWinMin = 0xFFFFFFFFu, s_dmaWinMin = 0xFFFFFFFFu, s_bigWinMin = 0xFFFFFFFFu;
            void sampleHeap()
            {
                const uint32_t f = ESP.getFreeHeap();
                const uint32_t d = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
                const uint32_t b = ESP.getMaxAllocHeap();
                if (f < s_heapWinMin) s_heapWinMin = f;
                if (d < s_dmaWinMin)  s_dmaWinMin  = d;
                if (b < s_bigWinMin)  s_bigWinMin  = b;
            }

            // The [task] line(s): every registered task, its stack never
            // touched (bytes), wakes in this window, and its longest pass in
            // this window. Several lines if they do not fit one LogBus line.
            void logTasks(uint32_t now)
            {
                static uint32_t s_wakesWas[Tasks::MAX] = {};
                char b[LogBus::MAX_LINE];
                int n = 0;
                const int count = Tasks::count();
                for (int i = 0; i < count; ++i) {
                    const TaskEntry* e = Tasks::at(i);
                    if (!e) continue;
                    const uint32_t wakes = e->wakes;
                    const uint32_t dw = wakes - s_wakesWas[i];
                    s_wakesWas[i] = wakes;
                    const unsigned stk = e->handle ? (unsigned)uxTaskGetStackHighWaterMark(e->handle) : 0u;
                    // Signed: the task may have beaten after `now` was read.
                    const int32_t age = (int32_t)(now - e->lastAliveMs);
                    char one[96];
                    // `*` marks a critical task: its age gates the OTA verdict.
                    const int m = snprintf(one, sizeof(one), " %s%s:stk=%u,w=%u,busy=%uus,age=%ums",
                                           e->name, e->critical ? "*" : "", stk, (unsigned)dw,
                                           (unsigned)e->busyMaxUs, (unsigned)(age < 0 ? 0 : age));
                    if (n && n + m >= (int)sizeof(b)) { LogBus::line(b); n = 0; }
                    if (!n) n = snprintf(b, sizeof(b), "[task]");
                    n += snprintf(b + n, sizeof(b) - n, "%s", one);
                }
                if (n) LogBus::line(b);
                Tasks::newWindow();
            }

            // OTA trial health: WiFi usable at least once since boot (the next
            // OTA needs it), heap above the floor, and every critical task in
            // the registry (the loop that owns the panel, keys, hu, knobs, scan
            // and this task) came round within the last 2 s: an image whose
            // keys or panel hang is not healthy however good its WiFi is.
            // Peripherals never gate it — no panel, no car, no head unit is a
            // parked car, not a bad build.
            bool healthy(uint32_t now)
            {
                const char* late = nullptr;
                const bool alive = Tasks::criticalAlive(now, 2000, &late);
                // Said once per change, so a trial that will not confirm says why.
                static bool s_wasAlive = true;
                if (alive != s_wasAlive) {
                    s_wasAlive = alive;
                    if (!alive) LogBus::printf("[task] %s has not run for over 2 s", late ? late : "?");
                    else        LogBus::printf("[task] every critical task is running again");
                }
                const bool wifi = s_h.wifiSeen ? s_h.wifiSeen() : true;
                return wifi && alive && ESP.getFreeHeap() > s_h.heapMin;
            }

            void run(void*)
            {
                uint32_t beatAt = millis(), slowAt = 0;
                bool slowDone = false;
                for (;;) {
                    const uint32_t t0 = micros();
                    const uint32_t now = millis();
                    if (s_h.web)  s_h.web(now);
                    if (s_h.wifi) s_h.wifi(now);
                    if (s_h.cloud) s_h.cloud(now, healthy(now));
                    sampleHeap();

                    if ((uint32_t)(now - beatAt) >= BEAT_MS) {
                        beatAt = now;
                        // `big` = largest free block (fragmentation).
                        LogBus::printf("[win] min=%u dma=%u big=%u  (10 s window, %u ms samples)",
                                       (unsigned)s_heapWinMin, (unsigned)s_dmaWinMin,
                                       (unsigned)s_bigWinMin, (unsigned)PASS_MS);
                        s_heapWinMin = s_dmaWinMin = s_bigWinMin = 0xFFFFFFFFu;
                        logTasks(now);
                        if (s_h.beat) s_h.beat(now);
                    }
                    if (!slowDone || (uint32_t)(now - slowAt) >= SLOW_MS) {
                        slowDone = true;
                        slowAt = now;
                        if (s_h.beatSlow) s_h.beatSlow(now);
                    }
                    Tasks::beat(s_id, micros() - t0);
                    vTaskDelay(pdMS_TO_TICKS(PASS_MS));
                }
            }
        }

        void start(const Hooks& h)
        {
            s_h = h;
            TaskSpec spec;
            spec.name     = "net_svc";
            spec.fn       = run;
            spec.stack    = Plan::NET_STACK;
            spec.prio     = Plan::NET_PRIO;
            spec.core     = Plan::NET_CORE;
            spec.critical = true;
            spec.idOut    = &s_id;
            s_task = spawn(spec);
        }

        TaskHandle_t task() { return s_task; }
    }
}
