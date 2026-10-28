#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <type_traits>
#ifndef NATIVE
#include <esp_heap_caps.h>
#endif

// A growable array of plain records (trivially copyable: decode rows,
// constants) for tables the owner can make as long as a car needs, up to a
// sanity cap: in PSRAM where the board has it, else the heap; the host build
// uses malloc. Copies are deep. An assignment that fits reuses the buffer, so
// a staged table copied over its twin keeps its address.
//
// Out of memory: push()/assign() return false and leave the array as it was.
template <class T>
class PsArray {
    static_assert(std::is_trivially_copyable<T>::value, "PsArray holds plain records");

public:
    PsArray() = default;
    ~PsArray() { release(_p); }
    PsArray(const PsArray& o) { assign(o); }
    PsArray& operator=(const PsArray& o)
    {
        if (this != &o) assign(o);
        return *this;
    }

    uint16_t size() const { return _n; }
    bool     empty() const { return _n == 0; }
    T*       data() { return _p; }
    const T* data() const { return _p; }
    T&       operator[](uint16_t i) { return _p[i]; }
    const T& operator[](uint16_t i) const { return _p[i]; }

    void clear() { _n = 0; }
    // `n` records, all zero.
    bool resize(uint16_t n)
    {
        if (!reserve(n)) return false;
        if (n) memset(_p, 0, sizeof(T) * n);
        _n = n;
        return true;
    }

    bool reserve(uint16_t cap)
    {
        if (cap <= _cap) return true;
        T* q = static_cast<T*>(alloc(sizeof(T) * cap));
        if (!q) return false;
        if (_n) memcpy(q, _p, sizeof(T) * _n);
        release(_p);
        _p = q;
        _cap = cap;
        return true;
    }

    bool assign(const PsArray& o) { return assign(o._p, o._n); }
    bool assign(const T* src, uint16_t n)
    {
        if (n > _cap) {
            T* q = static_cast<T*>(alloc(sizeof(T) * n));
            if (!q) return false;
            release(_p);
            _p = q;
            _cap = n;
        }
        if (n) memcpy(_p, src, sizeof(T) * n);
        _n = n;
        return true;
    }

    bool push(const T& v) { return insert(_n, v); }

    // Before index `at` (0.._n); the rest move up.
    bool insert(uint16_t at, const T& v)
    {
        if (at > _n || _n == 0xFFFF) return false;
        if (_n == _cap && !reserve(_cap ? (uint16_t)(_cap * 2 > 0xFFFF ? 0xFFFF : _cap * 2) : 8)) return false;
        if (at < _n) memmove(_p + at + 1, _p + at, sizeof(T) * (_n - at));
        _p[at] = v;
        ++_n;
        return true;
    }

    void erase(uint16_t at)
    {
        if (at >= _n) return;
        if (at + 1 < _n) memmove(_p + at, _p + at + 1, sizeof(T) * (_n - at - 1));
        --_n;
    }

private:
    static void* alloc(size_t n)
    {
#ifdef NATIVE
        return malloc(n);
#else
        void* m = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return m ? m : heap_caps_malloc(n, MALLOC_CAP_8BIT);
#endif
    }
    static void release(void* p)
    {
#ifdef NATIVE
        free(p);
#else
        heap_caps_free(p);
#endif
    }

    T*       _p = nullptr;
    uint16_t _n = 0, _cap = 0;
};
