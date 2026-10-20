#include "PaneBuffers.h"

namespace MegaOpen
{
    bool PaneBuffers::free(uint8_t s) const
    {
        return s < SLOTS && !_lent[s].load(std::memory_order_acquire);
    }

    bool PaneBuffers::lend(uint8_t s)
    {
        if (s >= SLOTS) return false;
        bool was = false;
        // Release ordering: the composed bytes are visible to the panel task
        // before it can see the slot as lent and hand it to the library.
        if (!_lent[s].compare_exchange_strong(was, true, std::memory_order_acq_rel)) return false;
        _lends.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void PaneBuffers::release(uint8_t s)
    {
        if (s >= SLOTS) return;
        if (!_lent[s].exchange(false, std::memory_order_acq_rel))
            _double.fetch_add(1, std::memory_order_relaxed);
    }

    uint8_t PaneBuffers::lentCount() const
    {
        uint8_t n = 0;
        for (uint8_t s = 0; s < SLOTS; ++s)
            if (_lent[s].load(std::memory_order_relaxed)) ++n;
        return n;
    }
}
