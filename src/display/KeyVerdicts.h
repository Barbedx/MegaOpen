#pragma once
#include <stdint.h>
#include <atomic>

// The verdicts of keys posted from another task (the web console's
// /api/display/key). Off the poll owner pressKey() hands back a ticket at once;
// the owner applies the key inside its next poll() and its verdict, Ok or why
// not (NotSupported, NoSync, LinkDown, SendFailed), arrives through onComplete
// under that ticket. This says which tickets are keys, so the verdict is
// reported as a key's (a refusal is logged) and not counted as a render's.
//
// The owner can complete a key before its poster has said it waits for it:
// they run on different cores. So the owner keeps its last few verdicts, and
// expect() looks there too. Whoever claims the waiting slot (a CAS on its
// ticket) reports, so each verdict is reported once. A key that completed
// before expect() has already been counted as a render's: only the counters
// can be off by it, never the report.
//
// Portable: atomics only, host-tested. Tickets are never 0 (kNoTicket).
namespace MegaOpen
{
    class KeyVerdicts {
    public:
        static constexpr uint8_t SLOTS = 4;

        // The poster, after pressKey() returned ticket `t`. True when its
        // verdict has already arrived: `result` holds it, report it now.
        // False: the owner reports it from complete(), or, with every slot
        // busy (SLOTS keys in flight), nobody does: then *unwatched is set
        // and unwatched() counts it, so the poster can say a refusal of this
        // key would go unlogged.
        bool expect(uint16_t t, uint8_t& result, bool* unwatched = nullptr)
        {
            if (!t) return false;
            for (std::atomic<uint16_t>& w : _wait) {
                uint16_t free = 0;
                if (!w.compare_exchange_strong(free, t)) continue;
                // Waiting now; but the verdict may have come first.
                if (!earlier(t, result)) return false;
                uint16_t mine = t;
                return w.compare_exchange_strong(mine, 0);
            }
            if (earlier(t, result)) return true;
            if (unwatched) *unwatched = true;
            _unwatched.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Keys whose verdict nobody watched (every slot busy at expect()).
        uint32_t unwatched() const { return _unwatched.load(std::memory_order_relaxed); }

        // The owner, in onComplete. True when `t` is a key's ticket somebody
        // waits for: the caller reports the verdict, and it is not a render's.
        bool complete(uint16_t t, uint8_t result)
        {
            if (!t) return false;
            // Kept first, then claimed: a poster between its expect() steps
            // sees one or the other (seq_cst on both sides), and the CAS on
            // the slot picks who reports.
            _done[_doneAt].store((uint32_t)t << 8 | result);
            _doneAt = (uint8_t)((_doneAt + 1) % SLOTS);
            for (std::atomic<uint16_t>& w : _wait) {
                uint16_t mine = t;
                if (w.compare_exchange_strong(mine, 0)) return true;
            }
            return false;
        }

    private:
        bool earlier(uint16_t t, uint8_t& result) const
        {
            for (const std::atomic<uint32_t>& d : _done) {
                const uint32_t v = d.load();
                if ((uint16_t)(v >> 8) != t) continue;
                result = (uint8_t)v;
                return true;
            }
            return false;
        }

        std::atomic<uint16_t> _wait[SLOTS] = {};
        std::atomic<uint32_t> _done[SLOTS] = {};   // ticket << 8 | result; the owner writes
        uint8_t               _doneAt = 0;          // the owner's alone
        std::atomic<uint32_t> _unwatched{0};
    };
}
