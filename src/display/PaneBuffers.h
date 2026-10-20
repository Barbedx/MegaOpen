#pragma once
#include <stdint.h>
#include <atomic>
#include "MediaPane.h"

// The two 288-byte images the pane is composed into (docs/design/event-tasks.md,
// Step 4). Portable: atomics only, host-tested with the mailbox ids.
//
// showNavBitmap() BORROWS its buffer: enqueueSplit() keeps the pointer and
// reads the bytes frame by frame as the transfer goes out, a few hundred ms
// later. So a buffer has two owners in turn, and this is the hand-over:
//
//   * the ui task composes only into a slot that is not lent;
//   * it lends the slot just before posting NavBitmap{slot} (and takes it back
//     itself if the post never left: the mailbox was full);
//   * the panel task releases it on that request's completion, and only then,
//     including the completions it synthesises for a refused call.
//
// The other slot keeps the last image sent, unchanged, which is what the ui
// compares a new composition against (the pane only sends on change).
namespace MegaOpen
{
    class PaneBuffers {
    public:
        static constexpr uint8_t  SLOTS = 2;
        static constexpr uint16_t BYTES = MediaPane::BYTES;

        uint8_t*       data(uint8_t s)       { return _buf[s < SLOTS ? s : 0]; }
        const uint8_t* data(uint8_t s) const { return _buf[s < SLOTS ? s : 0]; }

        // The ui: the slot's bytes are its own to write.
        bool free(uint8_t s) const;
        // The ui, before posting it. False when it is already lent.
        bool lend(uint8_t s);
        // The panel, on the completion of the request that borrowed it; or the
        // ui, when that request never reached the mailbox. A second release of
        // a free slot is counted and does nothing else.
        void release(uint8_t s);

        uint8_t  lentCount() const;
        uint32_t lends() const          { return _lends.load(std::memory_order_relaxed); }
        uint32_t doubleReleases() const { return _double.load(std::memory_order_relaxed); }

    private:
        uint8_t               _buf[SLOTS][BYTES] = {{0}, {0}};
        std::atomic<bool>     _lent[SLOTS] = {{false}, {false}};
        std::atomic<uint32_t> _lends{0}, _double{0};
    };
}
