#include "UiWaits.h"

namespace UiWaits
{
    bool Waits::add(Ticket t, uint8_t tag, uint32_t nowMs)
    {
        if (t == NO_TICKET) return false;
        for (Entry& e : _e) {
            if (e.t != NO_TICKET) continue;
            e.t = t;
            e.tag = tag;
            e.atMs = nowMs;
            return true;
        }
        ++_full;
        return false;
    }

    bool Waits::take(Ticket t, uint8_t& tag)
    {
        if (t == NO_TICKET) return false;
        for (Entry& e : _e) {
            if (e.t != t) continue;
            tag = e.tag;
            e = Entry{};
            return true;
        }
        return false;
    }

    void Waits::forget(Ticket t)
    {
        uint8_t tag;
        (void)take(t, tag);
    }

    bool Waits::expire(uint32_t nowMs, Ticket& t, uint8_t& tag)
    {
        for (Entry& e : _e) {
            if (e.t == NO_TICKET || (uint32_t)(nowMs - e.atMs) < TIMEOUT_MS) continue;
            t = e.t;
            tag = e.tag;
            e = Entry{};
            ++_timeouts;
            return true;
        }
        return false;
    }

    uint8_t Waits::used() const
    {
        uint8_t n = 0;
        for (const Entry& e : _e) if (e.t != NO_TICKET) ++n;
        return n;
    }
}
