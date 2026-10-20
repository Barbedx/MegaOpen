#pragma once
#include <stdint.h>
#include "Locks.h"

// One owner publishes a small struct; any task reads a whole, consistent copy
// (docs/design/event-tasks.md §3.1, rule 2). The house pattern already used by
// g_notifyMux and LogBus: a copy under a portMUX, nothing more.
//
// Not a seqlock, on purpose. A seqlock reader retries while a write is in
// progress, and on a single-core chip a reader that preempted the writer
// retries forever. The structs here are small, so the copy under the spinlock
// costs about a microsecond and nothing has to retry.
//
// seq() counts publishes, so a reader can tell "nothing new" without copying.
template <typename T>
class Snapshot {
    // The copy runs with interrupts off on this core. Kept small enough that
    // this stays a microsecond-scale section; a bigger struct wants a mutex
    // and a different pattern, not a bigger bound here.
    static_assert(sizeof(T) <= 256, "Snapshot<T> copies under a spinlock: keep T small");

public:
    // Owner only.
    void publish(const T& v)
    {
        _lock.lock();
        _v = v;
        ++_seq;
        _lock.unlock();
    }

    // Any task. Returns the publish count the copy belongs to.
    uint32_t read(T& out) const
    {
        _lock.lock();
        out = _v;
        const uint32_t s = _seq;
        _lock.unlock();
        return s;
    }

    T read() const
    {
        T v;
        read(v);
        return v;
    }

    // A single aligned word: no lock needed to see that something changed.
    uint32_t seq() const { return _seq; }

private:
    mutable Locks::Spin _lock;
    T                   _v{};
    volatile uint32_t   _seq = 0;
};
