#pragma once
#include <stdint.h>
#include "Locks.h"

// A copy of an object one task owns, for other tasks to read whole
// (docs/design/event-tasks.md, Step 4). Snapshot<T> does this under a spinlock
// and is capped at 256 bytes; the now-playing screen and the pane are bigger
// (~320 and ~630 bytes), and the web composes previews from them, which is far
// too long to keep interrupts off for. So this is the mutex version:
//
//   * the owner publishes with a zero-wait try-take, so it never waits for a
//     reader; a busy mirror is simply refreshed on the owner's next pass;
//   * a reader may wait for the mutex, and reads the copy in place (read(fn))
//     or copies it out.
//
// One extra copy of T, and no allocation. Portable (Locks.h stands in on the
// host).
template <typename T>
class Mirror {
public:
    // Owner. False when a reader held it (nothing written; try again later).
    bool publish(const T& v)
    {
        if (!_lock.tryTake()) { ++_skips; return false; }
        _v = v;
        ++_seq;
        _lock.give();
        return true;
    }

    // Any reader: fn(const T&) runs under the mutex, on the copy. Keep it to a
    // compose and a few field reads.
    template <typename F>
    bool read(F&& fn, uint32_t waitMs = 50) const
    {
        if (!_lock.take(waitMs)) return false;
        fn((const T&)_v);
        _lock.give();
        return true;
    }

    bool copy(T& out, uint32_t waitMs = 50) const
    {
        return read([&](const T& v) { out = v; }, waitMs);
    }

    // Publishes so far; 0 = never published.
    uint32_t seq() const   { return _seq; }
    uint32_t skips() const { return _skips; }

private:
    mutable Locks::Mutex _lock;
    T                    _v{};
    volatile uint32_t    _seq = 0;
    uint32_t             _skips = 0;    // owner only
};
