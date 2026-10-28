#pragma once
#include <stdint.h>
#include <stddef.h>
#include "KeyAction.h"

// Key combinations (combo, sequence, hold) mapped to one effect: a RAV4 key
// code, a raw Raise frame, a panel screen, a scheduled message.
//
// A filter, not a sink: it sits in KeyEngine::dispatch() ahead of the sinks
// and may consume an event (see `swallow`). Portable and host-testable; it
// names the effect and calls back, the executor lives with the hardware.
//
// One task only (docs/design/event-tasks.md, Step 2): the keys task runs
// observe(), tick() and run(), so there is no lock. The web edits a staged
// MacroTable and the keys task applies it with setTable().
namespace Keys
{
    enum class Trigger : uint8_t {
        Off = 0,
        Combo,      // N actions held down at the same time
        Sequence,   // one action pressed N times, each within windowMs of the last
        Hold,       // one action held past the debouncer's long threshold
        _Count
    };
    const char* triggerName(Trigger t);
    Trigger     triggerFromName(const char* s);

    // What firing does.
    enum class Effect : uint8_t {
        None = 0,
        Action,     // inject a logical action — goes through the whole routing matrix
        KeyCode,    // one literal RAV4 0x20 code straight down the UART
        Raw,        // an arbitrary Raise frame: `code` is the command, `data` the payload
        Text,       // three rows on the OEM panel; `arg` split on '|'
        Popup,      // a popup on the OEM panel
        Pane,       // the 48x48 graphic layer; `arg` is the scene name
        Message,    // the protocol emitter's sendNow(code): one scheduled telemetry message
        _Count
    };
    const char* effectName(Effect e);
    Effect      effectFromName(const char* s);

    // The names the API stores and /api/catalog serves (macroTriggers,
    // macroEffects); row i = enum value i, sized so a missing row won't compile.
    struct TriggerRow { const char* id; const char* name; bool taps; };
    struct EffectRow  { const char* id; const char* name; const char* field; };
    extern const TriggerRow TRIGGERS[(size_t)Trigger::_Count];
    extern const EffectRow  EFFECTS[(size_t)Effect::_Count];

    // Where an effect is carried out, which is where its executor must post
    // rather than act: the keys task does only what it owns.
    enum class EffectRoute : uint8_t {
        None = 0,
        Keys,      // Action, KeyCode: inline on the keys task (dispatch, the UART sink)
        UartOut,   // Raw: a frame for the hu task's vehicle lane
        Hu,        // Message: a one-shot request; the hu task owns the emitter
        Panel,     // Text, Popup: PanelOps (+ the popup timer request)
        Pane,      // Pane: a scene request to whoever owns the pane
    };
    EffectRoute effectRoute(Effect e);

    struct Macro {
        static constexpr uint8_t MAX_MEMBERS = 3;
        static constexpr uint8_t MAX_DATA    = 8;
        static constexpr uint8_t ARG_LEN     = 40;
        static constexpr uint8_t NAME_LEN    = 20;

        bool     enabled  = false;
        Trigger  trigger  = Trigger::Off;
        // Combo: the members, `members` of them. Sequence and Hold: keys[0].
        Action   keys[MAX_MEMBERS] = { Action::None, Action::None, Action::None };
        uint8_t  members  = 0;
        uint8_t  taps     = 3;      // Sequence only
        uint16_t windowMs = 600;    // Sequence: the most that may pass between taps

        // Keep the triggering presses off the sinks.
        // Combo: only partial — keys are never delayed (ARCHITECTURE.md §8), so
        // the first member is already dispatched; later presses are consumed and
        // a synthetic Release goes out for every member already down.
        // Sequence: complete — taps wait in `_pend`, replayed when the window
        // lapses or dropped on fire; costs that key up to windowMs latency.
        bool     swallow  = true;

        Effect   effect   = Effect::None;
        Action   action   = Action::None;   // Effect::Action
        // Effect::Action  — optional code override, 0 = the sink's own default
        // Effect::KeyCode — the RAV4 code
        // Effect::Raw     — the Raise command byte
        // Effect::Message — the emitter's message index
        uint8_t  code     = 0;
        uint8_t  data[MAX_DATA] = {};
        uint8_t  dataLen  = 0;
        char     arg[ARG_LEN]   = {0};
        char     name[NAME_LEN] = {0};

        // Diagnostics, never persisted.
        uint32_t fires    = 0;
        uint32_t lastMs   = 0;

        bool valid() const;
    };

    // The configuration, as the web edits it and the keys task applies it.
    struct MacroTable {
        static constexpr uint8_t COUNT = 8;
        Macro m[COUNT];

        // Two examples, both disabled: VOL+ & VOL- = SOURCE key code, MUTE x3 = popup.
        void loadDefaults();
        void clear() { for (uint8_t i = 0; i < COUNT; ++i) m[i] = Macro{}; }
    };

    class MacroEngine {
    public:
        static constexpr uint8_t  MAX_MACROS = MacroTable::COUNT;
        // Two events per tap (press + release) for the longest sequence
        // Macro::valid() accepts (10 taps), with a tap to spare. At 12 an 8- to
        // 10-tap macro overflowed on tap 7, replayed its taps and never fired.
        static constexpr uint8_t  PEND_MAX   = 2 * 10 + 2;
        static constexpr uint16_t BLOB_SIZE  = 1 + MAX_MACROS * 82;

        using FireFn = void (*)(const Macro& m, void* ctx);
        // Re-dispatch with the filter bypassed (replayed taps, synthetic
        // releases). Must not re-enter observe(), or a replayed tap restarts
        // its sequence.
        using EmitFn = void (*)(const Event& e, void* ctx);

        void onFire(FireFn f, void* ctx) { _fire = f; _fireCtx = ctx; }
        void onEmit(EmitFn f, void* ctx) { _emit = f; _emitCtx = ctx; }

        Macro&       at(uint8_t i)       { return _m[i < MAX_MACROS ? i : 0]; }
        const Macro& at(uint8_t i) const { return _m[i < MAX_MACROS ? i : 0]; }
        static constexpr uint8_t count() { return MAX_MACROS; }

        void clear();
        // Two disabled examples: VOL+ & VOL- = SOURCE key code, MUTE x3 = popup.
        void loadDefaults();

        // Replace the configuration (a staged table becoming live). The fire
        // counts are diagnostics, not configuration: each row keeps its own.
        // Half-collected sequences and mutes from the old table are dropped.
        void setTable(const MacroTable& t);
        void table(MacroTable& out) const;

        // Call after editing the table from the web UI: drops half-finished
        // sequences and any mute left over from a macro that no longer exists.
        void reset();

        // True when the event was consumed and must NOT reach the sinks.
        bool observe(const Event& e);

        // Perform one row's effect with no trigger: separates "not recognised"
        // from "sends the wrong thing". Leaves the counters alone.
        void run(uint8_t i);

        // Expires sequence windows. The keys task calls it at nextDue().
        void tick(uint32_t nowMs);
        // When held taps are due to be replayed; false when none are held.
        bool nextDue(uint32_t& dueMs) const { dueMs = _pendDeadline; return _pendN != 0; }

        uint32_t fired() const     { return _fired; }
        uint32_t swallowed() const { return _swallowed; }
        uint32_t replayed() const  { return _replayed; }
        // Bit per Action currently held; the page draws it live.
        uint32_t downMask() const  { return _down; }
        uint8_t  progress(uint8_t i) const;

        void toBlob(uint8_t* out) const;
        bool fromBlob(const uint8_t* in, uint16_t len);
        // The same format for a table on its own, for the web's staged copy.
        static void toBlob(const MacroTable& t, uint8_t* out);
        static bool fromBlob(MacroTable& t, const uint8_t* in, uint16_t len);

    private:
        struct Run {
            uint8_t  taps    = 0;
            uint32_t lastMs  = 0;
            bool     engaged = false;   // Combo/Hold: fired and not yet re-armed
        };

        bool matchCombo(const Event& e);
        bool matchSequence(const Event& e);
        bool matchHold(const Event& e);
        void fire(uint8_t i, const Event& e);
        void flush();                   // replay everything pending, in order
        void push(const Event& e);
        int8_t sequenceFor(Action a) const;

        Macro  _m[MAX_MACROS];
        Run    _r[MAX_MACROS];
        Event  _pend[PEND_MAX];
        uint8_t _pendN = 0;
        int8_t  _pendMacro = -1;
        uint32_t _pendDeadline = 0;

        uint32_t _down  = 0;    // bit per Action, currently held
        uint32_t _muted = 0;    // bit per Action, events dropped until it releases
        uint32_t _fired = 0, _swallowed = 0, _replayed = 0;

        FireFn _fire = nullptr;  void* _fireCtx = nullptr;
        EmitFn _emit = nullptr;  void* _emitCtx = nullptr;
    };
}
