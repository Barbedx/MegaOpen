#include "TlsJob.h"
#if FEATURE_GH_OTA || FEATURE_LOG_PUSH
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

namespace TlsJob
{
    namespace
    {
        portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
        const char*  s_who = nullptr;   // under s_mux

        // mbedTLS into PSRAM for the job: the prebuilt sdkconfig pins it to
        // internal RAM (MBEDTLS_INTERNAL_MEM_ALLOC), and a handshake's ~40 kB
        // there is most of what is free with BLE up.
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
    }

    bool claim(const char* who)
    {
        portENTER_CRITICAL(&s_mux);
        const bool ok = !s_who;
        if (ok) s_who = who;
        portEXIT_CRITICAL(&s_mux);
        return ok;
    }

    void release()
    {
        portENTER_CRITICAL(&s_mux);
        s_who = nullptr;
        portEXIT_CRITICAL(&s_mux);
    }

    const char* holder()
    {
        portENTER_CRITICAL(&s_mux);
        const char* w = s_who;
        portEXIT_CRITICAL(&s_mux);
        return w;
    }

    InPsram::InPsram()
    {
#if CONFIG_SPIRAM
        on = psramFound();
        if (on) mbedtls_platform_set_calloc_free(psramCalloc, heap_caps_free);
#endif
    }

    InPsram::~InPsram()
    {
        if (on) mbedtls_platform_set_calloc_free(internalCalloc, heap_caps_free);
    }
}
#endif
