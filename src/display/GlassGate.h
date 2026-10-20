#pragma once
#include <stdint.h>
#include <atomic>

// Who owns the glass, decided at the moment a render is taken (PanelOps).
//
// A person's screen (deliberate, and of an op that holds the glass) moves seq()
// when the library takes it. An automatic repaint carries the seq() its poster
// had seen and is refused if a person's screen was taken since. Checking that
// and then rendering are two steps, and a person's screen taken between them
// would be covered by the stale repaint; the library admits renders in the
// order they were taken. So both kinds run under one lock: the check, the
// render and the bump cannot interleave, and the order they leave in is the
// order the library admits them. Renders that neither yield nor hold the glass
// (popups, the clock) never take the lock.
//
// Portable: the lock is a parameter, so the host tests drive it with
// std::mutex and real threads. `Lock` has bool lock() (false = gave up waiting)
// and void unlock(). Only the library's render call runs under it: a copy into
// the dispatch ring, microseconds, never a wait.
namespace MegaOpen
{
    enum class GlassVerdict : uint8_t {
        Taken,      // the render ran and the library took it
        Refused,    // the render ran and the library refused it
        Yielded,    // not run: a person's screen was taken after `epoch`
        Busy,       // not run: the lock could not be had
    };

    template <class Lock>
    class GlassGate {
    public:
        // `render` is bool(): whether the library took it.
        template <class Render>
        GlassVerdict run(bool yields, uint16_t epoch, bool holdsGlass, Render&& render)
        {
            if (!yields && !holdsGlass) return render() ? GlassVerdict::Taken : GlassVerdict::Refused;
            if (!_lock.lock()) return GlassVerdict::Busy;
            GlassVerdict v;
            if (yields && epoch != _seq.load(std::memory_order_relaxed)) {
                _yielded.fetch_add(1, std::memory_order_relaxed);
                v = GlassVerdict::Yielded;
            } else if (render()) {
                if (holdsGlass) _seq.fetch_add(1, std::memory_order_release);
                v = GlassVerdict::Taken;
            } else {
                v = GlassVerdict::Refused;
            }
            _lock.unlock();
            return v;
        }

        // Person's screens taken; any task.
        uint16_t seq() const      { return _seq.load(std::memory_order_acquire); }
        // Repaints refused because one was taken after their epoch.
        uint32_t yielded() const  { return _yielded.load(std::memory_order_relaxed); }

    private:
        Lock                  _lock;
        std::atomic<uint16_t> _seq{0};
        std::atomic<uint32_t> _yielded{0};
    };
}
