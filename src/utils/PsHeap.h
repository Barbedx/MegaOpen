#pragma once
#include <stddef.h>
#include <stdlib.h>

// The one way to say "PSRAM, else internal RAM": the big, long-lived or
// request-scoped things (tables, scratch documents, a file's text) go to
// PSRAM where the board has it, and still work on a board without. Ten
// copies of these three lines used to exist, each a little different.
// heap_caps_free frees either, so the caller never needs to know which.
// Pure on the host (NATIVE): plain malloc.
#ifdef NATIVE
namespace PsHeap
{
    inline void* malloc(size_t n)             { return ::malloc(n); }
    inline void* calloc(size_t n, size_t sz)  { return ::calloc(n, sz); }
    inline void* aligned(size_t align, size_t n) { (void)align; return ::malloc(n); }
    inline void  free(void* p)                { ::free(p); }
}
#else
#include <esp_heap_caps.h>
namespace PsHeap
{
    constexpr uint32_t CAP_PS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    constexpr uint32_t CAP_IN = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    inline void* malloc(size_t n)             { return heap_caps_malloc_prefer(n, 2, CAP_PS, CAP_IN); }
    inline void* calloc(size_t n, size_t sz)  { return heap_caps_calloc_prefer(n, sz, 2, CAP_PS, CAP_IN); }
    inline void* aligned(size_t align, size_t n)
    {
        void* m = heap_caps_aligned_alloc(align, n, CAP_PS);
        return m ? m : heap_caps_aligned_alloc(align, n, CAP_IN);
    }
    inline void  free(void* p)                { heap_caps_free(p); }
}
#endif
