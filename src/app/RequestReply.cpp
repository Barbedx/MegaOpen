#include "RequestReply.h"

// The Table is portable and host-tested (test/test_runtime); the Channel
// below it is the FreeRTOS half and is compiled only for the target.
namespace RR
{
    bool Table::acquire(const Args& a, Ticket& t)
    {
        int pick = -1;
        for (uint8_t i = 0; i < SLOTS && pick < 0; ++i)
            if (_s[i].state == State::Free) pick = i;
        // An abandoned slot is fair game: its generation moves on below, so the
        // owner's late message for it is recognised as stale.
        for (uint8_t i = 0; i < SLOTS && pick < 0; ++i)
            if (_s[i].state == State::Abandoned) pick = i;
        if (pick < 0) return false;

        Slot& s = _s[pick];
        if (++s.gen == 0) s.gen = 1;          // 0 means "no ticket"
        s.state  = State::Pending;
        s.args   = a;
        s.result = Result{};
        t.idx = (uint8_t)pick;
        t.gen = s.gen;
        return true;
    }

    void Table::release(const Ticket& t)
    {
        if (matches(t) && _s[t.idx].state == State::Pending) _s[t.idx].state = State::Free;
    }

    bool Table::take(const Ticket& t, Args& out)
    {
        if (!matches(t)) { ++_stale; return false; }
        Slot& s = _s[t.idx];
        if (s.state == State::Abandoned) {
            // Nobody is waiting: skip the work, and the slot is ours to free.
            s.state = State::Free;
            ++_stale;
            return false;
        }
        if (s.state != State::Pending) { ++_stale; return false; }
        out = s.args;
        return true;
    }

    bool Table::complete(const Ticket& t, const Result& r)
    {
        if (!matches(t)) { ++_stale; return false; }
        Slot& s = _s[t.idx];
        if (s.state == State::Abandoned) {
            // Gave up while the owner was working on it.
            s.state = State::Free;
            ++_stale;
            return false;
        }
        if (s.state != State::Pending) { ++_stale; return false; }
        s.result = r;
        s.state  = State::Done;
        return true;
    }

    bool Table::collect(const Ticket& t, Result& out)
    {
        if (!matches(t) || _s[t.idx].state != State::Done) return false;
        out = _s[t.idx].result;
        _s[t.idx].state = State::Free;
        return true;
    }

    bool Table::abandon(const Ticket& t)
    {
        if (!matches(t)) return true;
        Slot& s = _s[t.idx];
        if (s.state == State::Done) return false;     // finished meanwhile: collect it
        if (s.state == State::Pending) s.state = State::Abandoned;
        return true;
    }

    Table::Settle Table::settle(const Ticket& t, bool woke, Result& out)
    {
        // An answer counts whether or not we were woken for it: the owner may
        // have finished just as the wait ran out.
        if (collect(t, out)) return Settle::Answer;
        if (woke) return Settle::KeepWaiting;
        abandon(t);                                   // cannot be Done: collect just failed
        return Settle::GaveUp;
    }
}

#ifndef NATIVE
namespace RR
{
    void Channel::begin()
    {
        for (uint8_t i = 0; i < SLOTS; ++i)
            if (!_sem[i]) _sem[i] = xSemaphoreCreateBinaryStatic(&_semBuf[i]);
    }

    Channel::Call Channel::call(const Args& a, Result& out, PostFn post, void* ctx,
                                uint32_t timeoutMs)
    {
        Ticket t;
        _mux.lock();
        const bool got = _t.acquire(a, t);
        _mux.unlock();
        if (!got) return Call::NoSlot;

        // A give left over from a waiter that timed out just as its answer
        // arrived; without this drain it would wake us before the owner ran.
        xSemaphoreTake(_sem[t.idx], 0);

        if (!post || !post(t, ctx)) {
            _mux.lock();
            _t.release(t);
            _mux.unlock();
            return Call::PostFailed;
        }

        // A stray give can still land after the drain above (the owner was
        // between its unlock and its give for the slot's previous holder), so
        // a wake is only a hint: settle() decides, and after a stray one we
        // wait out whatever is left of the timeout.
        const TickType_t start = xTaskGetTickCount();
        const TickType_t total = pdMS_TO_TICKS(timeoutMs);
        for (;;) {
            const TickType_t spent = xTaskGetTickCount() - start;
            const bool woke = spent < total
                && xSemaphoreTake(_sem[t.idx], total - spent) == pdTRUE;
            _mux.lock();
            const Table::Settle s = _t.settle(t, woke, out);
            _mux.unlock();
            if (s == Table::Settle::Answer) return Call::Ok;
            if (s == Table::Settle::GaveUp) { ++_timeouts; return Call::Timeout; }
        }
    }

    bool Channel::take(const Ticket& t, Args& out)
    {
        _mux.lock();
        const bool ok = _t.take(t, out);
        _mux.unlock();
        return ok;
    }

    void Channel::complete(const Ticket& t, const Result& r)
    {
        _mux.lock();
        const bool wake = _t.complete(t, r);
        _mux.unlock();
        if (wake) xSemaphoreGive(_sem[t.idx]);
    }

    uint32_t Channel::stale()
    {
        _mux.lock();
        const uint32_t n = _t.stale();
        _mux.unlock();
        return n;
    }
}
#endif
