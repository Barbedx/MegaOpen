#pragma once
#include <stdint.h>
#include "Locks.h"

// A config that httpd edits and one owner task uses (docs/design/event-tasks.md
// §3.1, rule 3). It exists because the web handlers used to write straight
// into objects a key or render was reading at that moment, Strings included.
//
// Two preallocated buffers: `live`, which only the owner reads, and `staged`,
// which writers fill under the mutex. The owner calls applyIfDirty() on its own
// schedule; that is a zero-wait try-take and a pointer swap, so the owner never
// blocks behind a web request and never copies or allocates on its hot path.
// A writer may wait for the mutex; only the owner refuses to.
//
// After applyIfDirty() returns true the owner must drop any reference it held
// into the old live(): that buffer is now the staging one and the next edit
// writes into it.
//
// External = true: the two buffers are the caller's, handed over once with
// place() before any other call, for a T big enough to want PSRAM (the decode
// map). Everything else is the same.
template <typename T, bool External = false>
class Staged {
public:
    // Before any other task can touch it (setup()).
    void init(const T& v)
    {
        *_live = v;
        *_staged = v;
        _dirty = false;
    }

    // External storage only: two buffers, allocated once and never freed.
    void place(T* a, T* b)
    {
        static_assert(External, "place() is for Staged<T, true>");
        _live = a;
        _staged = b;
        _dirty = false;
    }

    // ---- owner -----------------------------------------------------------------
    const T& live() const { return *_live; }

    // Never blocks: if a writer holds the mutex, the change lands on the next
    // call. True when a new value became live.
    bool applyIfDirty()
    {
        if (!_dirty) return false;
        if (!_lock.tryTake()) { ++_busySkips; return false; }
        T* t = _live;
        _live = _staged;
        _staged = t;
        _dirty = false;
        ++_applied;
        _lock.give();
        return true;
    }

    // ---- writers (httpd, setup) -----------------------------------------------
    // Replace the whole value.
    bool stage(const T& v, uint32_t waitMs = WRITER_WAIT_MS)
    {
        if (!_lock.take(waitMs)) return false;
        *_staged = v;
        _dirty = true;
        _lock.give();
        return true;
    }

    // Read-modify-write. fn(T&) starts from the newest value — a change still
    // waiting to be applied, else the live one — so two edits in a row both
    // land even if the owner applies neither in between.
    template <typename F>
    bool edit(F&& fn, uint32_t waitMs = WRITER_WAIT_MS)
    {
        if (!_lock.take(waitMs)) return false;
        if (!_dirty) *_staged = *_live;
        fn(*_staged);
        _dirty = true;
        _lock.give();
        return true;
    }

    // The newest value, applied or not: what a settings page should show.
    bool peek(T& out, uint32_t waitMs = WRITER_WAIT_MS)
    {
        if (!_lock.take(waitMs)) return false;
        out = _dirty ? *_staged : *_live;
        _lock.give();
        return true;
    }

    // The same, in place: fn(const T&) runs under the mutex, for a T too big
    // to copy onto an httpd stack (the macro table). Keep fn short.
    template <typename F>
    bool read(F&& fn, uint32_t waitMs = WRITER_WAIT_MS)
    {
        if (!_lock.take(waitMs)) return false;
        fn(_dirty ? (const T&)*_staged : (const T&)*_live);
        _lock.give();
        return true;
    }

    bool     dirty() const     { return _dirty; }
    uint32_t applied() const   { return _applied; }
    // Times the owner found a writer mid-edit and moved on.
    uint32_t busySkips() const { return _busySkips; }

    static constexpr uint32_t WRITER_WAIT_MS = 50;

private:
    template <typename U, bool Ext> struct Store { U a{}, b{}; U* pa() { return &a; } U* pb() { return &b; } };
    template <typename U> struct Store<U, true> { U* pa() { return nullptr; } U* pb() { return nullptr; } };

    Store<T, External> _store;
    T*            _live = _store.pa();
    T*            _staged = _store.pb();
    volatile bool _dirty = false;
    uint32_t      _applied = 0, _busySkips = 0;   // owner only
    Locks::Mutex  _lock;
};
