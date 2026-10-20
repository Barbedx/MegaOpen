#pragma once
#include "KeyAction.h"

// A destination for key events (ARCHITECTURE.md §6): CanboxUartSink (Raise 0x20
// over the head-unit UART), ResistiveKeySink (KEY1/KEY2), WebSink (live debug
// stream). Sinks are registered with KeyRouter under a bit and can be enabled
// per action class from the web UI.
//
// THE TIME CONTRACT (docs/design/event-tasks.md §3.4). Event.tMs is when the key
// happened at its source — the panel's decode, the knob's sample, the matrix
// sweep — not when the keys task got round to it. A key can sit in the KeyBus
// for a few milliseconds, so a sink that schedules a release must not count
// its pulse from tMs alone: a queued detent would get a shortened release, or
// one that is already due and goes out on the heels of its press, which some
// head units drop. releaseDue() is the rule every scheduling sink uses.
namespace Keys
{
    // The shortest gap between a press leaving and its scheduled release.
    constexpr uint32_t MIN_GAP_MS = 20;

    // max(tMs + pulse, now + MIN_GAP_MS), wrap-safe: the pulse is measured from
    // the source, but never ends sooner than MIN_GAP_MS from now.
    inline uint32_t releaseDue(uint32_t tMs, uint32_t pulseMs, uint32_t nowMs)
    {
        const uint32_t fromSource = tMs + pulseMs;
        const uint32_t earliest   = nowMs + MIN_GAP_MS;
        return (int32_t)(fromSource - earliest) >= 0 ? fromSource : earliest;
    }

    class ISink {
    public:
        virtual ~ISink() = default;

        // Deliver one event. Must not block: called from the key path, which is
        // the highest-priority traffic in the system. Queue and return.
        virtual void onKey(const Event& e) = 0;

        // Short stable id used in config and in the UI ("uart", "res", "web").
        virtual const char* id() const = 0;
    };
}
