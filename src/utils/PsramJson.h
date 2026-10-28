#pragma once
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// An ArduinoJson allocator that puts a document's pools in PSRAM, internal
// RAM when there is none (heap_caps_free frees either). For documents built
// beside a TLS job, where the internal heap is what the handshake needs.
struct PsramJson final : ArduinoJson::Allocator {
    static constexpr uint32_t kPs = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    static constexpr uint32_t kIn = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    void* allocate(size_t n) override { return heap_caps_malloc_prefer(n, 2, kPs, kIn); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc_prefer(p, n, 2, kPs, kIn); }
    static PsramJson& instance() { static PsramJson a; return a; }
};
