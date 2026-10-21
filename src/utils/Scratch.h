#pragma once
#include <esp_heap_caps.h>
#include <new>

// Per-request working space for the web routes: in PSRAM where the board has
// it, else the heap; constructed fresh (value-initialised) and freed when the
// handler returns. Static buffers there cost internal RAM for the life of the
// board; these exist only while a request runs. Out of memory: !scratch, and
// the route answers 503.
template <class T>
class Scratch {
public:
    Scratch()
    {
        void* m = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!m) m = heap_caps_malloc(sizeof(T), MALLOC_CAP_8BIT);
        _p = m ? new (m) T() : nullptr;
    }
    ~Scratch() { if (_p) { _p->~T(); heap_caps_free(_p); } }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    explicit operator bool() const { return _p != nullptr; }
    T* operator->() { return _p; }
    T* get() { return _p; }
    T& operator*() { return *_p; }

private:
    T* _p = nullptr;
};
