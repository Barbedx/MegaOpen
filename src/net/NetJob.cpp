#include "NetJob.h"
#if FEATURE_GH_OTA || FEATURE_LOG_PUSH
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "../app/TaskPlan.h"
#include "../app/TaskRegistry.h"
#include "../utils/HeapSentinel.h"
#include "../utils/LogBus.h"

namespace NetJob
{
    namespace
    {
        struct Job { const char* name; Fn fn; void* arg; Fn after; };
        constexpr uint8_t QUEUE_LEN = 4;

        // ESP-IDF's StackType_t is a byte: the depth below is in bytes, as
        // xTaskCreate's is. The stack is internal RAM (a task stack must be:
        // the OTA install writes flash with the cache off), taken from the
        // heap ONCE, at the first job, and kept. Not static: 10 kB of BSS
        // moved the whole heap down, and the OTA trial's health floor
        // (HEALTH_HEAP_MIN) was never met (bench boot 196, 1.8.0-rc1). The
        // trial holds the push, so a trial never pays for it.
        static_assert(sizeof(StackType_t) == 1, "the stack depth below is in bytes");
        StaticTask_t  s_tcb;
        StackType_t*  s_stack = nullptr;
        StaticQueue_t s_qbuf;
        uint8_t       s_qstore[QUEUE_LEN * sizeof(Job)];
        QueueHandle_t s_q = nullptr;
        TaskHandle_t  s_task = nullptr;
        int           s_id = -1;        // the task registry's row
        const char* volatile s_running = nullptr;
        volatile bool s_tlsOn = false;

        // mbedTLS into PSRAM for the job: the prebuilt sdkconfig pins it to
        // internal RAM (MBEDTLS_INTERNAL_MEM_ALLOC), and a handshake's ~40 kB
        // there is most of what is free with BLE up. heap_caps_free frees
        // either, so the swap back is safe with blocks still out.
        void* psramCalloc(size_t n, size_t size)
        {
            return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        // What IDF's own allocator does under MBEDTLS_INTERNAL_MEM_ALLOC; its
        // header declares it without extern "C", so C++ cannot link to it.
        void* internalCalloc(size_t n, size_t size)
        {
            return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        struct InPsram {
            bool on = false;
            InPsram()
            {
#if CONFIG_SPIRAM
                on = psramFound();
                if (on) mbedtls_platform_set_calloc_free(psramCalloc, heap_caps_free);
#endif
            }
            ~InPsram()
            {
                if (on) mbedtls_platform_set_calloc_free(internalCalloc, heap_caps_free);
            }
        };

        void taskBody(void*)
        {
            for (;;) {
                Job j;
                if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE) continue;
                const uint32_t t0 = micros();
                s_running = j.name;
                HEAP_CHECK("net job start");
                {
                    InPsram tls;
                    s_tlsOn = tls.on;
                    j.fn(j.arg);
                    s_tlsOn = false;
                }
                if (j.after) j.after(j.arg);
                HEAP_CHECK("net job done");
                s_running = nullptr;
                if (s_id >= 0) App::Tasks::beat(s_id, micros() - t0);
            }
        }
    }

    void begin()
    {
        if (s_q) return;
        s_q = xQueueCreateStatic(QUEUE_LEN, sizeof(Job), s_qstore, &s_qbuf);
        // Not critical: a job that hangs on the network must never cost the
        // keys an OTA rollback; it shows as age in the [task] line.
        s_id = App::Tasks::add("netjob", false);
    }

    // The task, at the first job: its stack from the internal heap, kept.
    static bool ensureTask()
    {
        if (s_task) return true;
        if (!s_stack) s_stack = (StackType_t*)heap_caps_malloc(App::Plan::NETJOB_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_stack) return false;
        s_task = xTaskCreateStaticPinnedToCore(taskBody, "netjob", App::Plan::NETJOB_STACK, nullptr,
                                               App::Plan::NETJOB_PRIO, s_stack, &s_tcb,
                                               App::core(App::Plan::NETJOB_CORE));
        if (s_task && s_id >= 0) App::Tasks::setHandle(s_id, s_task);
        return s_task != nullptr;
    }

    bool post(const char* name, Fn fn, void* arg, Fn after)
    {
        if (!s_q || !fn || !ensureTask()) return false;
        const Job j{name, fn, arg, after};
        return xQueueSend(s_q, &j, 0) == pdTRUE;
    }

    const char* running() { return s_running; }
    uint8_t     waiting() { return s_q ? (uint8_t)uxQueueMessagesWaiting(s_q) : 0; }
    bool        busy()    { return s_running != nullptr || waiting() > 0; }
    bool        tlsInPsram() { return s_tlsOn; }
}
#endif
