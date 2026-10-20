#pragma once
#include <stdint.h>
#include "../utils/Locks.h"

// A request slot one task posts and one owner task takes (docs/design/event-
// tasks.md, Step 2). It is how a task asks for something done to state it does
// not own — "arm the popup for 2 s", "show this pane scene", "the head unit
// says it is 14:05" — without writing that state itself, which is what used
// to happen: the keys path and httpd stored straight into the loop's render
// state while the loop was in the middle of using it.
//
// One slot, latest wins: a newer request replaces one the owner has not taken
// yet (counted, so it shows if it ever matters). Every request here is a
// "set to" and never an "add", so losing the older one loses nothing.
//
// The copy runs under a spinlock, so T must be a few bytes. Portable.
template <typename T>
class OneShot {
    static_assert(sizeof(T) <= 32, "OneShot<T> copies under a spinlock: keep T small");

public:
    // Any task.
    void post(const T& v)
    {
        _lock.lock();
        if (_full) ++_overwritten;
        _v = v;
        _full = true;
        ++_posted;
        _lock.unlock();
    }

    // The owner. False when nothing is waiting; the plain read first keeps the
    // common empty case off the lock.
    bool take(T& out)
    {
        if (!_full) return false;
        _lock.lock();
        const bool had = _full;
        if (had) { out = _v; _full = false; }
        _lock.unlock();
        return had;
    }

    bool     pending() const     { return _full; }
    uint32_t posted() const      { return _posted; }
    uint32_t overwritten() const { return _overwritten; }

private:
    mutable Locks::Spin _lock;
    T                   _v{};
    volatile bool       _full = false;
    uint32_t            _posted = 0, _overwritten = 0;
};
