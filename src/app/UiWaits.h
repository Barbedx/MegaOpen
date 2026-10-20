#pragma once
#include <stdint.h>

// The renders the ui task is waiting on (docs/design/event-tasks.md, Step 4).
// Portable: no FreeRTOS, no AffaDisplay, so the host tests can drive it and
// env:direct (no panel) still compiles it.
//
// The ui task renders from its own task; the library hands back a ticket at the
// call and its verdict later, through onComplete on the panel task, which
// forwards every one as a Completion. This table says which of them the ui
// still waits on and what each was for, and gives a wait up after TIMEOUT_MS:
// a verdict that never comes (UiQ full) must not leave the clock, the pane or
// the splash waiting for ever.
//
//   Ticket  the library's TxTicket (uint16, kNoTicket = 0), the same type here
//           so this file needs no library include.
namespace UiWaits
{
    using Ticket = uint16_t;
    constexpr Ticket NO_TICKET = 0;      // affa::kNoTicket

    struct Completion {
        Ticket  ticket = NO_TICKET;
        uint8_t result = 0;          // affa::Result; 0 = Ok
    };

    class Waits {
    public:
        static constexpr uint8_t  CAP = 8;
        // Longer than the library can legitimately keep a ticket, or a
        // delivered render is declared Timeout, rendered again, and its real
        // verdict lands as a stray. With the AffaConfig defaults: hold 8000 +
        // ACK 2000 x (1 + 3 retries) + backoff 250+500+1000 + dirty quiet
        // 400 x 3 = 18 950 ms, plus ~2 s for the queue ahead of it. This file
        // includes no library header (host tests), so UiTask.cpp
        // static_asserts it against AffaConfig.
        static constexpr uint32_t LIB_WORST_MS = 18950;
        static constexpr uint32_t TIMEOUT_MS = LIB_WORST_MS + 2050;   // 21 000

        // False when the table is full (counted): the caller treats the render
        // as answered rather than wait on something it cannot time out.
        bool add(Ticket t, uint8_t tag, uint32_t nowMs);
        // The verdict arrived. False for a ticket nobody waits on (given up,
        // replaced, or not the ui's): ignore it.
        bool take(Ticket t, uint8_t& tag);
        // Forget a wait without a verdict (a coalesced repaint replaced it).
        void forget(Ticket t);
        // One overdue wait, freed. Call until it returns false.
        bool expire(uint32_t nowMs, Ticket& t, uint8_t& tag);

        uint8_t  used() const;
        uint32_t timeouts() const { return _timeouts; }
        uint32_t full() const     { return _full; }

    private:
        struct Entry {
            Ticket   t = NO_TICKET;
            uint8_t  tag = 0;
            uint32_t atMs = 0;
        };
        Entry    _e[CAP];
        uint32_t _timeouts = 0, _full = 0;
    };
}
