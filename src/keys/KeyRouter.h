#pragma once
#include <stdint.h>
#include "KeyAction.h"

// The policy half of the KeyBus (docs/design/event-tasks.md §3.2, §3.4): what a
// key message is, which order the keys task takes them in, and what happens to
// a release that could not be queued. Portable, no FreeRTOS: src/app/KeyBus
// puts the queues under a portMUX and wakes the keys task; everything that can
// be got wrong lives here, where the host tests can drive it.
//
// Why there is one dispatcher at all: before this, the loop, the scan task and
// httpd all ran the sink chain at once, and the UART sink's release table and
// the macro state were written by whichever got there first.
namespace Keys
{
    // One item on the bus. Plain bytes only — no affa::Key, no <AffaDisplay.h>
    // — so env:direct, which has no panel library, builds the bus unchanged.
    struct KeyMsg {
        enum class Kind : uint8_t {
            Event = 0,   // one event as the source produced it (a matrix edge, a detent)
            Tap,         // a press AND its release, as one item (knob button, web test)
            PanelKey,    // the panel's raw key: AffaKeySource maps it on the keys task
            Literal,     // a literal RAV4 code straight to the UART sink (web)
            MacroRun,    // perform one macro's effect, no trigger (web)
        };
        Kind     kind = Kind::Event;
        // PanelKey: 1 = hold. Literal: the state byte. MacroRun: the row.
        uint8_t  a = 0;
        // Literal: 1 = also send the release.
        uint8_t  b = 0;
        // PanelKey: the wire code. Literal: the RAV4 code.
        uint16_t code = 0;
        // Source time in microseconds (esp_timer, low 32 bits): what the
        // latency histogram measures from. Differences are wrap-safe.
        uint32_t atUs = 0;
        // PanelKey: microseconds since the poll() before the one that decoded
        // it — how long the frame could have waited in the TWAI queue.
        uint32_t aux = 0;
        // Arrival order across all lanes, stamped by KeyQueues::push.
        uint32_t seq = 0;
        // Event and Tap: the event. PanelKey: tMs is the source time.
        Event    ev;
    };

    // Release first, then presses, then the web. A release is never stuck
    // behind a spin of detents, and web traffic can never starve the keys.
    // A release does not jump a real key's press that arrived before it, so
    // keys keep their order among themselves (a combo still sees both down).
    enum class Lane : uint8_t { Rel = 0, Tap = 1, Web = 2 };
    constexpr uint8_t LANES = 3;
    const char* laneName(Lane l);

    // The three lanes as fixed rings, with the order the keys task takes them
    // in. Not thread-safe: the firmware calls every method under one lock.
    class KeyQueues {
    public:
        static constexpr uint8_t REL_DEPTH = 16;
        static constexpr uint8_t TAP_DEPTH = 32;
        static constexpr uint8_t WEB_DEPTH = 4;
        static constexpr uint8_t LOST_MAX  = 8;
        static constexpr uint8_t ORIGINS   = 8;     // srcId 0..7, for the per-origin drops

        // Stamps m.seq. False when the lane is full: the message is dropped and
        // counted. A dropped RELEASE is also remembered (see takeLost), because
        // a lost press is a missed key but a lost release is a stuck one.
        bool push(Lane l, KeyMsg m);

        // The next message to handle. The Rel lane goes first, past any
        // encoder detents queued before it, but never past a press of a real
        // key (its own or another's) that arrived first: those go in arrival
        // order, and the release after them.
        bool pop(KeyMsg& out);

        bool    empty() const { return !_rel.n && !_tap.n && !_web.n; }
        uint8_t size(Lane l) const;
        uint32_t drops(Lane l) const { return _drops[(uint8_t)l]; }
        uint32_t tapDropsFrom(uint8_t srcId) const { return srcId < ORIGINS ? _tapDropsBy[srcId] : 0; }

        // Releases that a full Rel lane dropped, as (srcId, action) in an Event
        // with State::Release. Taken once; the keys task synthesises the ones
        // whose press it actually dispatched (KeyRouter::synthesise).
        uint8_t takeLost(Event* out, uint8_t max);

        // The same, but only those that may be synthesised now: no item older
        // than the lost release is left in the Rel or Tap lane, so every press
        // queued before it has been handled. Traffic queued after it does not
        // hold it back. `releaseAll` is set (once) when more releases were
        // lost than LOST_MAX could name and the last of them is ready too: the
        // keys task then lets go of every key still down.
        uint8_t takeLostReady(Event* out, uint8_t max, bool& releaseAll);

        // Releases dropped with no room left to remember them.
        uint32_t lostOverflow() const { return _lostOverflow; }

    private:
        // No Rel or Tap item older than `seq` is left.
        bool olderGone(uint32_t seq) const;

        template <uint8_t N>
        struct Ring {
            KeyMsg  buf[N];
            uint8_t head = 0, n = 0;
            bool full() const { return n >= N; }
            void push(const KeyMsg& m) { buf[(uint8_t)((head + n) % N)] = m; ++n; }
            const KeyMsg& at(uint8_t i) const { return buf[(uint8_t)((head + i) % N)]; }
            void pop(KeyMsg& out) { out = buf[head]; head = (uint8_t)((head + 1) % N); --n; }
        };

        Ring<REL_DEPTH> _rel;
        Ring<TAP_DEPTH> _tap;
        Ring<WEB_DEPTH> _web;
        uint32_t _seq = 0;
        uint32_t _drops[LANES] = {};
        uint32_t _tapDropsBy[ORIGINS] = {};
        Event    _lost[LOST_MAX];
        uint32_t _lostSeq[LOST_MAX] = {};
        uint8_t  _lostN = 0;
        uint32_t _lostOverflow = 0;
        bool     _lostAll = false;      // an unnamed release is owed
        uint32_t _lostAllSeq = 0;       // ...and the latest of them
    };

    // Which keys are down right now, per origin and action, as the sinks saw
    // them. Fed from the engine's event hook, so it sees exactly what was
    // dispatched: a press a macro swallowed is not down. Detents never are.
    class DownSet {
    public:
        static constexpr uint8_t MAX = 8;
        void note(const Event& e);
        bool has(uint8_t srcId, Action a) const;
        uint8_t size() const { return _n; }
        // Presses that did not fit: their release still goes through, it just
        // cannot be synthesised if it is lost.
        uint32_t overflow() const { return _overflow; }
        // Key i (i < size()) as the Release that lets it go, with the code
        // override its press carried.
        Event releaseOf(uint8_t i) const;
    private:
        struct Key { uint8_t src; Action a; uint8_t code; };
        Key      _k[MAX];
        uint8_t  _n = 0;
        uint32_t _overflow = 0;
    };

    // The keys task's handling of one message: what it expands to, in order.
    class KeyRouter {
    public:
        struct Hooks {
            // The engine's dispatch: macro filter, then the sinks.
            void (*dispatch)(const Event& e, void* ctx) = nullptr;
            // AffaKeySource: map a raw panel code. tMs is the source time.
            void (*panelKey)(uint16_t code, bool hold, uint32_t tMs, void* ctx) = nullptr;
            // The UART sink's sendKey, with an optional release.
            void (*literal)(uint8_t code, uint8_t state, bool release, void* ctx) = nullptr;
            // MacroEngine::run.
            void (*macroRun)(uint8_t idx, void* ctx) = nullptr;
            void* ctx = nullptr;
        };
        void begin(const Hooks& h) { _h = h; }

        // A Tap is its press and then its release, back to back, so its
        // release can never be lost on its own. A detent has no release.
        void handle(const KeyMsg& m);

        // Every event any path dispatched (KeyEngine's event hook).
        void observe(const Event& e) { _down.note(e); }

        // Releases the queues dropped: send each one whose key is still down.
        // Call only with releases KeyQueues::takeLostReady handed out, so every
        // press queued before them has been handled. Returns how many went out.
        uint8_t synthesise(const Event* lost, uint8_t n, uint32_t nowMs);

        // More releases were lost than could be named: let go of every key
        // still down. Returns how many went out.
        uint8_t releaseAll(uint32_t nowMs);

        uint32_t stuckSynth() const { return _stuckSynth; }
        const DownSet& down() const { return _down; }

    private:
        Hooks    _h;
        DownSet  _down;
        uint32_t _stuckSynth = 0;
    };
}
