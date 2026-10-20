#include "MailboxIds.h"

namespace MailboxIds
{
    ReqId IdGen::next()
    {
        // fetch_add wraps; a 0 is skipped by taking the next one, which two
        // racing posters may both do — they still get different ids.
        uint16_t v = (uint16_t)(_n.fetch_add(1, std::memory_order_relaxed) + 1);
        if (v == NO_REQ) v = (uint16_t)(_n.fetch_add(1, std::memory_order_relaxed) + 1);
        return v;
    }

    bool TicketMap::afterRender(ReqId id, uint8_t slot, bool ok, uint8_t result,
                                Ticket before, Ticket after, Completion& out)
    {
        out = Completion{};
        out.id = id;
        out.slot = slot;

        // Refused: no ticket now, none later. The reason is the call's result.
        // Accepted with nothing new enqueued (lastEnqueued() did not move): no
        // verdict will ever come for it either.
        if (!ok || after == NO_TICKET || after == before) {
            out.result = result;
            out.synth = true;
            ++_synth;
            return true;
        }

        // Completed inside its own call: its verdict is already in hand.
        if (_early != NO_TICKET && _early == after) {
            _early = NO_TICKET;
            out.result = _earlyResult;
            return true;
        }

        // A stale entry under the same ticket (it wrapped round) is replaced.
        Entry* freeE = nullptr;
        for (Entry& e : _e) {
            if (e.t == after) { freeE = &e; break; }
            if (!freeE && e.t == NO_TICKET) freeE = &e;
        }
        if (!freeE) {
            // Cannot happen with the library's bounded queue; if it does, the
            // ui stops waiting and the slot comes back now.
            ++_full;
            out.result = result;
            out.synth = true;
            return true;
        }
        freeE->t = after;
        freeE->id = id;
        freeE->slot = slot;
        ++_mapped;
        return false;
    }

    Completion TicketMap::dropped(ReqId id, uint8_t slot, uint8_t result)
    {
        Completion c;
        c.id = id;
        c.slot = slot;
        c.result = result;
        c.synth = true;
        return c;
    }

    bool TicketMap::complete(Ticket t, uint8_t result, Completion& out)
    {
        if (t == NO_TICKET) return false;
        for (Entry& e : _e) {
            if (e.t != t) continue;
            out = Completion{};
            out.id = e.id;
            out.slot = e.slot;
            out.result = result;
            e = Entry{};
            return true;
        }
        // Untracked (a web render, a key popup), or not mapped yet.
        _early = t;
        _earlyResult = result;
        ++_unknown;
        return false;
    }

    uint8_t TicketMap::used() const
    {
        uint8_t n = 0;
        for (const Entry& e : _e) if (e.t != NO_TICKET) ++n;
        return n;
    }

    bool Waits::add(ReqId id, uint8_t tag, uint32_t nowMs)
    {
        if (id == NO_REQ) return false;
        for (Entry& e : _e) {
            if (e.id != NO_REQ) continue;
            e.id = id;
            e.tag = tag;
            e.atMs = nowMs;
            return true;
        }
        ++_full;
        return false;
    }

    bool Waits::take(ReqId id, uint8_t& tag)
    {
        if (id == NO_REQ) return false;
        for (Entry& e : _e) {
            if (e.id != id) continue;
            tag = e.tag;
            e = Entry{};
            return true;
        }
        return false;
    }

    void Waits::forget(ReqId id)
    {
        uint8_t tag;
        (void)take(id, tag);
    }

    bool Waits::expire(uint32_t nowMs, ReqId& id, uint8_t& tag)
    {
        for (Entry& e : _e) {
            if (e.id == NO_REQ || (uint32_t)(nowMs - e.atMs) < TIMEOUT_MS) continue;
            id = e.id;
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
        for (const Entry& e : _e) if (e.id != NO_REQ) ++n;
        return n;
    }
}
