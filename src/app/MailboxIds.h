#pragma once
#include <stdint.h>
#include <atomic>

// Which posted request a panel verdict belongs to (docs/design/event-tasks.md,
// Step 4, §3.6). Portable: no FreeRTOS, no AffaDisplay, so the host tests can
// drive every order the library's callbacks can arrive in, and env:direct
// (no panel) still compiles it.
//
// The render policy (the ui task) posts to the mailbox and never calls the
// library, so it can no longer read lastEnqueued() after its own call. Instead
// every post gets a ReqId, and the panel task, which does make the call, ties
// the library's ticket to that id. When the library reports the ticket's
// verdict, the panel turns it back into Completion{id, result} and posts it to
// the ui task.
//
//   ReqId   issued by the mailbox on post(). uint16, never 0 (0 = not posted).
//   Ticket  the library's TxTicket (uint16, kNoTicket = 0), the same type here
//           so this file needs no library include.
//
// Three rules carry the whole protocol:
//   * A call the library refuses (kNoTicket: BadArgument, TooLong, NoSync...)
//     has no ticket and never will, so its completion is synthesised on the
//     spot, `synth` set. So is an accepted call that enqueued nothing.
//   * A coalescing render completes the ticket it replaces (Aborted) from
//     INSIDE the new call. That older ticket was mapped when it was drained,
//     so it still finds its id; only then is the new ticket mapped.
//   * A request that borrowed a pane buffer names its slot, and the slot comes
//     back with the completion, synthesised ones included. That is the only
//     moment the buffer may be written again: enqueueSplit() keeps the pointer,
//     not a copy.
namespace MailboxIds
{
    using ReqId  = uint16_t;
    using Ticket = uint16_t;
    constexpr ReqId   NO_REQ    = 0;
    constexpr Ticket  NO_TICKET = 0;      // affa::kNoTicket
    constexpr uint8_t NO_SLOT   = 0xFF;

    struct Completion {
        ReqId   id     = NO_REQ;
        uint8_t result = 0;          // affa::Result; 0 = Ok
        uint8_t slot   = NO_SLOT;    // the pane buffer it borrowed, free again now
        bool    synth  = false;      // never reached the wire: refused, or nothing to send
    };

    // Any task. Wraps past 0xFFFF and skips 0, so an id is never "not posted".
    class IdGen {
    public:
        ReqId next();
    private:
        std::atomic<uint16_t> _n{0};
    };

    // The panel task's half: ticket -> request, for the requests the ui waits on.
    class TicketMap {
    public:
        // The library has at most AFFA_TX_QUEUE_DEPTH (6) tickets outstanding,
        // and every one of them completes (begin() fails the whole queue), so
        // a correctly working build never comes near this.
        static constexpr uint8_t CAP = 16;

        // Before the render call made for a tracked request: forgets any
        // earlier unclaimed verdict, so only one from inside this call counts.
        void beforeRender() { _early = NO_TICKET; }

        // After the render call made for a tracked request. `before` and `after`
        // are lastEnqueued() around the call; `ok` is whether it returned Ok.
        // True when the request's completion is due NOW (filled in `out`):
        // refused, accepted with nothing enqueued, completed inside the call,
        // or the table full (counted; the slot is handed back rather than
        // wedging the pane for good).
        bool afterRender(ReqId id, uint8_t slot, bool ok, uint8_t result,
                         Ticket before, Ticket after, Completion& out);

        // A request that was never made, by the panel's own decision (an
        // automatic repaint a deliberate screen overtook). Always synthesised.
        static Completion dropped(ReqId id, uint8_t slot, uint8_t result);

        // The library's verdict. True when the ticket belongs to a tracked
        // request; its entry is freed.
        bool complete(Ticket t, uint8_t result, Completion& out);

        uint8_t  used() const;
        uint32_t mapped() const      { return _mapped; }
        uint32_t synthesised() const { return _synth; }
        uint32_t unknown() const     { return _unknown; }   // untracked or stray tickets
        uint32_t full() const        { return _full; }

    private:
        struct Entry {
            Ticket  t    = NO_TICKET;
            ReqId   id   = NO_REQ;
            uint8_t slot = NO_SLOT;
        };
        Entry    _e[CAP];
        // The last verdict nobody claimed. A ticket that completed inside its
        // own render call (the library does not do this today) is matched
        // against it, so it cannot leave an entry that never completes.
        Ticket   _early = NO_TICKET;
        uint8_t  _earlyResult = 0;
        uint32_t _mapped = 0, _synth = 0, _unknown = 0, _full = 0;
    };

    // The ui task's half: the requests it is waiting on, each given up after
    // TIMEOUT_MS. A completion that never comes (UiQ full, a lost id) must not
    // leave the clock, the pane or the splash waiting for ever.
    class Waits {
    public:
        static constexpr uint8_t  CAP = 8;
        // Longer than the library can legitimately keep a tracked ticket, or a
        // delivered request is declared Timeout, re-posted, and its real
        // verdict lands as a stray. With the AffaConfig
        // defaults: hold 8000 + ACK 2000 x (1 + 3 retries) + backoff
        // 250+500+1000 + dirty quiet 400 x 3 = 18 950 ms, plus ~2 s for the
        // mailbox and the queue ahead of it. This file includes no library
        // header (host tests), so UiTask.cpp static_asserts it against AffaConfig.
        static constexpr uint32_t LIB_WORST_MS = 18950;
        static constexpr uint32_t TIMEOUT_MS = LIB_WORST_MS + 2050;   // 21 000

        // False when the table is full (counted): the caller treats the request
        // as answered rather than wait on something it cannot time out.
        bool add(ReqId id, uint8_t tag, uint32_t nowMs);
        // The verdict arrived. False for an id nobody waits on (given up, or
        // replaced): ignore it.
        bool take(ReqId id, uint8_t& tag);
        // Forget a wait without a verdict (a coalesced repaint replaced it).
        void forget(ReqId id);
        // One overdue wait, freed. Call until it returns false.
        bool expire(uint32_t nowMs, ReqId& id, uint8_t& tag);

        uint8_t  used() const;
        uint32_t timeouts() const { return _timeouts; }
        uint32_t full() const     { return _full; }

    private:
        struct Entry {
            ReqId    id = NO_REQ;
            uint8_t  tag = 0;
            uint32_t atMs = 0;
        };
        Entry    _e[CAP];
        uint32_t _timeouts = 0, _full = 0;
    };
}
