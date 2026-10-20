#pragma once
#include "../keys/IKeySink.h"
#include "ResistiveMap.h"

// KEY1/KEY2 resistive keys (ARCHITECTURE.md §6.2): a resistance on the line
// that the head unit's key-study page learns like a real wheel. Protocol-free,
// so it can run beside the UART sink.
//
// 1. A level is not an event: Long and Repeat are dropped; the rung is already
//    held and the head unit does its own long-press and auto-repeat.
// 2. One owner per channel: two rungs at once read as a third resistance.
// 3. A detent has no release: a step is press-now/release-later, and a fast
//    spin becomes a queue (drain policy: ResistiveMap::Spin).
//
// Portable and host-tested: output via callback, time via tick().
namespace Sinks
{
    class ResistiveKeySink : public Keys::ISink {
    public:
        // Engage `mask` on channel `ch`. Mask 0 means every leg open.
        using ApplyFn = void (*)(uint8_t ch, uint8_t mask, void* ctx);

        // Deepest queued spin: ~1.5 s of fast wheel at the default pulse.
        // Beyond that, drop (counted) rather than deliver a stale backlog.
        static constexpr uint8_t QUEUE_MAX = 12;

        void begin(ApplyFn apply, void* ctx, ResistiveMap* map);

        // `enabled` = legs fitted; `classes` = which presses route here.
        void setEnabled(bool on);
        bool enabled() const { return _enabled; }
        void setClasses(uint8_t m) { _mask = m; }
        uint8_t classes() const { return _mask; }

        void onKey(const Keys::Event& e) override;
        const char* id() const override { return "res"; }

        // Retire pulses, enforce gaps, drain the queue, break stuck keys.
        // Call every few ms.
        void tick(uint32_t nowMs);

        // Open both channels now. Used when the sink is disabled or routing
        // changes under a held key, so no resistance is left behind.
        void allOff();

        // ---- learn assist ----
        // The DUDU study page needs a steady hold of >3 s then open circuit.
        // A detent is a 60 ms pulse and can't be held, so without this the
        // wheel cannot be taught. Asserts one rung for `ms`, bypassing routing
        // (so a button can be taught before its class is on) and the queue.
        void learnAssert(uint8_t ch, uint8_t mask, uint32_t ms, uint32_t nowMs);
        bool learnAction(Keys::Action a, uint32_t ms, uint32_t nowMs);
        // Also what a second learn request on the same channel does.
        void learnCancel(uint32_t nowMs);
        bool learning(uint8_t ch) const {
            return ch < ResistiveMap::CHANNELS && _ch[ch].learn;
        }

        // Documented 3 s plus 1 s margin.
        static constexpr uint32_t LEARN_MS = 4000;

        // What this event would do; shares onKey()'s resolver so the page
        // can't disagree with the wire.
        struct Explain {
            bool     acts  = false;
            uint8_t  ch    = 0;
            uint8_t  mask  = 0;
            uint16_t ohms  = ResistiveLadder::OPEN;
            bool     queued = false;    // a detent: goes to the queue, not the line
            const char* why = "";       // why not, when acts is false
        };
        Explain explain(const Keys::Event& e) const;

        // ---- counters ----
        // dropped:  queue lost steps (Pulse mode, fast spin)
        // preempts: a button took the channel from the wheel
        // stuck:    guard fired; non-zero means Release events go missing upstream
        uint32_t presses()  const { return _presses; }
        uint32_t detents()  const { return _detents; }
        uint32_t dropped()  const { return _dropped; }
        uint32_t preempts() const { return _preempts; }
        uint32_t stuck()    const { return _stuck; }
        // Presses that took the long rung.
        uint32_t longs()    const { return _longs; }

        // Live readout for the settings page.
        uint8_t  liveMask(uint8_t ch) const { return ch < ResistiveMap::CHANNELS ? _ch[ch].mask : 0; }
        uint16_t liveOhms(uint8_t ch) const;
        uint8_t  queued(uint8_t ch)   const { return ch < ResistiveMap::CHANNELS ? _ch[ch].queue : 0; }

    private:
        struct Channel {
            uint8_t      mask     = 0;      // applied right now
            Keys::Action owner    = Keys::Action::None;
            bool         held     = false;  // waiting for a Release of its own
            uint32_t     untilMs  = 0;      // pulse end, or the stuck-key deadline
            uint32_t     freeAtMs = 0;      // nothing may engage before this
            uint8_t      queue    = 0;
            Keys::Action queueAction = Keys::Action::None;
            uint32_t     holdEndMs = 0;     // Spin::Hybrid cap
            // Learn assert: not preemptible by keys; ends on deadline or cancel.
            bool         learn    = false;
            // Button with a long rung, down but undecided; nothing on the wire
            // yet. See onKey().
            Keys::Action pending  = Keys::Action::None;
        };

        void engage(uint8_t ch, uint8_t mask, Keys::Action a, bool held, uint32_t nowMs);
        void open(uint8_t ch, uint32_t nowMs);
        void startNextDetent(uint8_t ch, uint32_t nowMs);
        uint16_t pulseFor(uint8_t depth) const;
        uint16_t gapFor(uint8_t depth) const;

        ApplyFn       _apply = nullptr;
        void*         _ctx   = nullptr;
        ResistiveMap* _map   = nullptr;
        bool          _enabled = false;
        // Off by default: until the resistors are fitted and learned, any rung
        // is a press the driver didn't make.
        uint8_t       _mask = Keys::CLASS_NONE;

        Channel  _ch[ResistiveMap::CHANNELS];
        uint32_t _presses = 0, _detents = 0, _dropped = 0, _preempts = 0, _stuck = 0, _longs = 0;
    };
}
