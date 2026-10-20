#pragma once
#include <stdint.h>

// A web request that needs an answer from the task that owns the hardware
// (docs/design/event-tasks.md §3.3). httpd claims a slot, posts its index and
// generation to the owner's queue, and waits up to 2 s for the owner to fill in
// the result. Nothing in a slot points into httpd's stack, so a handler that
// gave up and returned cannot be written through afterwards.
//
// The generation is what makes giving up safe. A waiter that times out marks
// its slot Abandoned; the owner, when it finally gets to the message, finds the
// slot abandoned (and frees it) or already reused under a newer generation (and
// leaves it alone). Either way it writes no result that nobody will read.
namespace RR
{
    constexpr uint8_t SLOTS = 4;

    // Small and fixed so a slot is plain data. `op` names the operation; what
    // the words mean is the op's business.
    struct Args {
        uint8_t  op = 0;
        uint32_t u[4] = {};
    };
    struct Result {
        int32_t  status = 0;
        uint32_t u[8] = {};
    };

    struct Ticket {
        uint8_t  idx = 0xFF;
        uint16_t gen = 0;          // never 0 for a real slot
        bool valid() const { return idx < SLOTS && gen != 0; }
    };

    enum class State : uint8_t { Free, Pending, Done, Abandoned };

    // The bookkeeping, with no locks and no FreeRTOS, so the host tests can
    // drive every interleaving. Not thread-safe on its own: the firmware
    // Channel below calls each method under one portMUX.
    class Table {
    public:
        // Waiter: claim a slot for `a`. A Free slot first, else an Abandoned
        // one, whose owner message then turns stale. False when all are busy.
        bool acquire(const Args& a, Ticket& t);
        // Waiter: the post to the owner failed; the slot was never seen.
        void release(const Ticket& t);
        // Owner: the work behind a posted ticket. False when the waiter gave up
        // (the slot is freed here) or the slot was reused: do nothing then.
        bool take(const Ticket& t, Args& out);
        // Owner: the answer. True when a waiter is still there to collect it,
        // which is when the owner signals it.
        bool complete(const Ticket& t, const Result& r);
        // Waiter, once signalled: copy the answer and free the slot.
        bool collect(const Ticket& t, Result& out);
        // Waiter, on timeout. True: abandoned, no answer will come. False: the
        // owner finished in the meantime, and collect() has the answer.
        bool abandon(const Ticket& t);

        // Waiter, after every wait; `woke` says the semaphore was taken. A wake
        // with no answer behind it is a stray give (the owner's late signal to
        // an earlier holder of this slot), so it means "keep waiting", never
        // "give up": giving up there would leave the slot Pending, the owner
        // would later set it Done, and nothing ever frees a Done slot.
        enum class Settle : uint8_t { Answer, KeepWaiting, GaveUp };
        Settle settle(const Ticket& t, bool woke, Result& out);

        State    state(uint8_t idx) const { return idx < SLOTS ? _s[idx].state : State::Free; }
        uint16_t gen(uint8_t idx) const   { return idx < SLOTS ? _s[idx].gen : 0; }
        // Owner messages that arrived for a slot already given up or reused.
        uint32_t stale() const { return _stale; }

    private:
        struct Slot {
            uint16_t gen = 0;
            State    state = State::Free;
            Args     args;
            Result   result;
        };
        bool matches(const Ticket& t) const { return t.valid() && _s[t.idx].gen == t.gen; }

        Slot     _s[SLOTS];
        uint32_t _stale = 0;
    };
}

#ifndef NATIVE
#include "../utils/Locks.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace RR
{
    // The firmware half: the Table under a portMUX, and one binary semaphore
    // per slot for the owner to wake the waiter with. One Channel per owner
    // task; created before httpd can call it.
    class Channel {
    public:
        // Zero-wait post of the ticket to the owner's queue.
        using PostFn = bool (*)(const Ticket& t, void* ctx);
        enum class Call : uint8_t { Ok, NoSlot, PostFailed, Timeout };

        void begin();

        // httpd. Blocks this handler (only) for up to timeoutMs.
        Call call(const Args& a, Result& out, PostFn post, void* ctx,
                  uint32_t timeoutMs = 2000);

        // Owner.
        bool take(const Ticket& t, Args& out);
        void complete(const Ticket& t, const Result& r);

        uint32_t timeouts() const { return _timeouts; }
        uint32_t stale();

    private:
        Table             _t;
        Locks::Spin       _mux;
        StaticSemaphore_t _semBuf[SLOTS];
        SemaphoreHandle_t _sem[SLOTS] = {};
        volatile uint32_t _timeouts = 0;
    };
}
#endif
