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

// The same for `n` raw bytes, 8-aligned, uninitialised: room for an object
// whose type only its module knows (Canbox::Protocol::makeEmitter).
class ScratchBytes {
public:
    explicit ScratchBytes(size_t n)
    {
        _p = heap_caps_aligned_alloc(8, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!_p) _p = heap_caps_aligned_alloc(8, n, MALLOC_CAP_8BIT);
    }
    ~ScratchBytes() { if (_p) heap_caps_free(_p); }
    ScratchBytes(const ScratchBytes&) = delete;
    ScratchBytes& operator=(const ScratchBytes&) = delete;
    explicit operator bool() const { return _p != nullptr; }
    void* get() { return _p; }

private:
    void* _p = nullptr;
};
