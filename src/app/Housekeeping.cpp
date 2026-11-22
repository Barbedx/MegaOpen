#include "Housekeeping.h"
#include "Rt.h"
#include "OtaTrial.h"
#include "../utils/LogBus.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include "../utils/HeapSentinel.h"

namespace App
{
    namespace Housekeeping
    {
        namespace
        {
            // The heap sampler's period; the largest block, a heap walk, every
            // BIG_EVERY samples. net_svc's own passes are Plan::NET_PASS_MS.
            constexpr uint32_t SAMPLE_MS = 20;
            constexpr uint32_t BIG_EVERY = 10;
            constexpr uint32_t BEAT_MS = 10000;
            constexpr uint32_t SLOW_MS = 60000;

            Hooks        s_h;
            int          s_id = -1;
            TaskHandle_t s_task = nullptr;

            // Heap low-water marks per heartbeat window (ESP.getMinFreeHeap()
            // is since boot). DMA free is separate: WiFi buffers live there.
            // `at` = when the free minimum was taken, `ctx` = what ran then
            // (Hooks::ctx), so a dip names its cause (stabilisation 1.12, S3).
            struct Win { uint32_t min, dma, big, at, ctx; };
            constexpr Win WIN_EMPTY = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0};
            Win          s_win = WIN_EMPTY;   // the sampler writes, net_svc takes
            portMUX_TYPE s_winMux = portMUX_INITIALIZER_UNLOCKED;
            esp_timer_handle_t s_sampler = nullptr;

            // Every SAMPLE_MS, on the esp_timer task (a foreign context, R1):
            // it only samples, so net_svc need not wake 50 times a second to
            // keep the 20 ms resolution of [win] (S7).
            void sampleHeap(void*)
            {
                static uint32_t s_n = 0;
                const uint32_t now = millis();
                const uint32_t f = ESP.getFreeHeap();
                const uint32_t d = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
                const bool bigDue = s_n++ % BIG_EVERY == 0;
                const uint32_t b = bigDue ? ESP.getMaxAllocHeap() : 0xFFFFFFFFu;
                // Only this writes the minima, so the context is asked outside
                // the lock; a window taken in between costs one stale compare.
                const bool newMin = f < s_win.min;
                const uint32_t c = newMin && s_h.ctx ? s_h.ctx() : 0;
                portENTER_CRITICAL(&s_winMux);
                if (f < s_win.min) { s_win.min = f; s_win.at = now; s_win.ctx = c; }
                if (d < s_win.dma) s_win.dma = d;
                if (b < s_win.big) s_win.big = b;
                portEXIT_CRITICAL(&s_winMux);
            }

            Win takeWindow()
            {
                portENTER_CRITICAL(&s_winMux);
                const Win w = s_win;
                s_win = WIN_EMPTY;
                portEXIT_CRITICAL(&s_winMux);
                return w;
            }

            // "job,http,ble1,join", or "-".
            void ctxText(uint32_t c, char* out, size_t n)
            {
                int k = 0;
                out[0] = 0;
                auto add = [&](const char* s) {
                    if (k < (int)n) k += snprintf(out + k, n - k, "%s%s", k ? "," : "", s);
                };
                if (c & CTX_JOB)  add("job");
                if (c & CTX_HTTP) add("http");
                if (c >> 8 & 0xFF) {
                    char b[8];
                    snprintf(b, sizeof(b), "ble%u", (unsigned)(c >> 8 & 0xFF));
                    add(b);
                }
                if (c & CTX_JOIN) add("join");
                if (!k) snprintf(out, n, "-");
            }

            // W [mem] low, once per episode: a window whose minimum is under
            // Plan::HEAP_LOW; quiet again after a window above it.
            void memGuard(const Win& w, const char* ctx)
            {
                static bool s_low = false;
                const bool low = w.min < Plan::HEAP_LOW;
                if (low && !s_low)
                    LOGW("mem", "low min=%u big=%u at=%u ctx=%s limit=%u", (unsigned)w.min,
                         (unsigned)w.big, (unsigned)w.at, ctx, (unsigned)Plan::HEAP_LOW);
                s_low = low;
            }

            // The [task] line(s): every registered task, its stack never
            // touched (bytes), wakes in this window, and its longest pass in
            // this window. Several lines if they do not fit one LogBus line.
            void logTasks(uint32_t now)
            {
                static uint32_t s_wakesWas[Tasks::MAX] = {};
                static int8_t s_taskTag = -1;
                const bool say = Log::statusOn("task", s_taskTag, now);
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
                    if (e->handle) stackGuard(e->name, stk);
                    // Signed: the task may have beaten after `now` was read.
                    const int32_t age = (int32_t)(now - e->lastAliveMs);
                    char one[96];
                    // `*` marks a critical task: its age gates the OTA verdict.
                    const int m = snprintf(one, sizeof(one), " %s%s:stk=%u,w=%u,busy=%uus,age=%ums",
                                           e->name, e->critical ? "*" : "", stk, (unsigned)dw,
                                           (unsigned)e->busyMaxUs, (unsigned)(age < 0 ? 0 : age));
                    if (n && n + m >= (int)sizeof(b)) { if (say) Log::write(LogLevel::INF, "task", "%s", b + 1); n = 0; }
                    n += snprintf(b + n, sizeof(b) - n, "%s", one);
                }
                if (n && say) Log::write(LogLevel::INF, "task", "%s", b + 1);
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
                    if (!alive) LOGI("task", "%s has not run for over 2 s", late ? late : "?");
                    else        LOGI("task", "every critical task is running again");
                }
                const bool wifi = s_h.wifiSeen ? s_h.wifiSeen() : true;
                return wifi && alive && ESP.getFreeHeap() > OtaTrial::HEALTH_HEAP_MIN;
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
                    OtaTrial::tick(now, healthy(now));
                    if (!s_sampler) sampleHeap(nullptr);   // no timer: at this pass's rate
#if HEAP_SENTINEL
                    static uint32_t s_walkAt = 0;
                    if ((uint32_t)(now - s_walkAt) >= 500) { s_walkAt = now; HEAP_CHECK("tick"); }
#endif

                    if ((uint32_t)(now - beatAt) >= BEAT_MS) {
                        beatAt = now;
                        // `big` = largest free block (fragmentation); `at` =
                        // uptime of the free minimum, `ctx` = what ran then.
                        // A failed allocation says its task in its own line
                        // (AllocWatch::report, `[mem] alloc failed`).
                        const Win w = takeWindow();
                        char ctx[40];
                        ctxText(w.ctx, ctx, sizeof(ctx));
                        LOGS("win", "min=%u dma=%u big=%u at=%u ctx=%s  (10 s window, %u ms samples)",
                                       (unsigned)w.min, (unsigned)w.dma, (unsigned)w.big,
                                       (unsigned)w.at, ctx, (unsigned)(s_sampler ? SAMPLE_MS : Plan::NET_PASS_MS));
                        memGuard(w, ctx);
                        logTasks(now);
                        if (s_h.beat) s_h.beat(now);
                    }
                    if (!slowDone || (uint32_t)(now - slowAt) >= SLOW_MS) {
                        slowDone = true;
                        slowAt = now;
                        if (s_h.beatSlow) s_h.beatSlow(now);
#if HEAP_SENTINEL
                        LOGI("heap", "sentinel: %u walks, the longest %u us", (unsigned)HeapSentinel::checks(), (unsigned)HeapSentinel::maxUs());
#endif
                    }
                    Tasks::beat(s_id, micros() - t0);
                    // To the next deadline: the heartbeat, the slow line, or
                    // NET_PASS_MS for the web, WiFi and the verdict (S7; it
                    // was 20 ms, for the heap sampling the timer does now).
                    const uint32_t after = millis();
                    int32_t wait = (int32_t)Plan::NET_PASS_MS;
                    const int32_t toBeat = (int32_t)(BEAT_MS - (after - beatAt));
                    const int32_t toSlow = (int32_t)(SLOW_MS - (after - slowAt));
                    if (toBeat < wait) wait = toBeat;
                    if (toSlow < wait) wait = toSlow;
                    const TickType_t ticks = wait > 0 ? pdMS_TO_TICKS((uint32_t)wait) : 0;
                    vTaskDelay(ticks ? ticks : 1);
                }
            }
        }

        void start(const Hooks& h)
        {
            s_h = h;
            esp_timer_create_args_t a = {};
            a.callback = sampleHeap;
            a.dispatch_method = ESP_TIMER_TASK;
            a.name = "heapwin";
            if (esp_timer_create(&a, &s_sampler) != ESP_OK ||
                esp_timer_start_periodic(s_sampler, SAMPLE_MS * 1000) != ESP_OK) {
                s_sampler = nullptr;
                LOGW("win", "no heap sampler timer: [win] samples at net_svc's %u ms",
                     (unsigned)Plan::NET_PASS_MS);
            }
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

        void stackGuard(const char* task, uint32_t freeBytes)
        {
            // One episode flag per name; net_svc only (the [task] and [stk]
            // lines both run on it).
            struct Seen { const char* name; bool low; };
            static Seen s_seen[Tasks::MAX + 8] = {};
            if (!task) return;
            Seen* slot = nullptr;
            for (Seen& x : s_seen) {
                if (!x.name) { x.name = task; slot = &x; break; }
                if (x.name == task || strcmp(x.name, task) == 0) { slot = &x; break; }
            }
            if (!slot) return;   // table full: no guard for this one
            const bool low = freeBytes < Plan::STACK_LOW;
            if (low && !slot->low)
                LOGW("stk", "low task=%s free=%u limit=%u", task, (unsigned)freeBytes, (unsigned)Plan::STACK_LOW);
            slot->low = low;
        }

        int coreStacks(char* out, size_t n)
        {
            // configMAX_TASK_NAME_LEN is 16: "arduino_usb_events" is
            // registered as its first 15 characters.
            static const char* const NAMES[] = {"nimble_host", "arduino_usb_eve", "arduino_events",
                                                "usbd", "wifi", "tiT"};
            int k = 0;
            if (n) out[0] = 0;
            for (const char* name : NAMES) {
                if (k >= (int)n) break;
                // By name every time, never a kept handle: a task the core
                // deletes (BLE or WiFi deinit) must not be read through one.
                const TaskHandle_t h = xTaskGetHandle(name);
                if (h) {
                    const uint32_t f = (uint32_t)uxTaskGetStackHighWaterMark(h);
                    stackGuard(name, f);
                    k += snprintf(out + k, n - k, " %s=%u", name, (unsigned)f);
                } else {
                    k += snprintf(out + k, n - k, " %s=-", name);
                }
            }
            return k < (int)n ? k : (int)n - 1;
        }
    }
}
