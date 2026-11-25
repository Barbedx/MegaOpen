#pragma once
#include <stddef.h>
#include <stdint.h>
#include "Expr.h"
#include "VehicleState.h"
#include "../keys/GestureEngine.h"
#include "../utils/PsArray.h"

// Rules (docs/design/rules.md, slice 4): one event mechanism for what the panel
// alerts and the key macros did up to 1.6. A rule is WHEN -> DO, with a pause
// and an on/off switch:
//
//   * a SIGNAL rule's trigger is a boolean formula (Expr.h) over the car's
//     signals: it fires when the formula goes from false to true (true when
//     first seen counts: the lamp already lit at ignition), re-arms when it
//     goes false. There is no "set a signal" action and no ELSE: a state a
//     rule would keep is a formula (a user signal), and "when it clears" is a
//     second rule on !(...) or falls(...).
//   * a GESTURE rule's trigger is a key gesture GestureEngine detects on the keys
//     task (a combo, N taps, a hold); its formula, when there is one, is
//     "only if": the gesture fires only while it holds. The keys task posts
//     the row to the car task (App::Car::fireRule), which checks and performs.
//
// The actions are the macros' effects (Keys::Effect): a popup on the panel,
// three rows of text, a pane scene, a key action through the routing matrix, a
// literal key code, a raw head-unit frame, a head-unit message. Up to three,
// in order. The engine only says WHICH rule fired; the car task performs the
// popup itself (one pending, when the glass is free) and posts the rest to
// the task that owns each (App::RuleActions::perform, by Keys::effectRoute).
//
// The rows live in the car file (/cfg/car.json "rules", CarFile) and in
// PSRAM. Portable: env:native tests it (test_rules).
namespace Vehicle
{
    constexpr uint8_t RULE_NAME = 31;

    // One action: a macro's effect half, the fields Keys::EFFECTS[].field names.
    struct RuleAction {
        Keys::Effect effect = Keys::Effect::None;
        Keys::Action action = Keys::Action::None;             // Action
        uint8_t      code = 0;                                 // Action's code override, KeyCode, Raw's command, Message's index
        uint8_t      data[Keys::Gesture::MAX_DATA] = {};         // Raw's payload
        uint8_t      dataLen = 0;
        char         arg[Keys::Gesture::ARG_LEN] = {0};          // Text ('|' between rows), Popup ({signal} placeholders), Pane (the scene)
        // The effect has what it needs (Gesture::valid's effect half).
        bool valid() const;
    };

    // The gesture half of a macro. Trigger Off: the rule is a signal rule.
    struct RuleGesture {
        Keys::Trigger trigger = Keys::Trigger::Off;
        Keys::Action  keys[Keys::Gesture::MAX_MEMBERS] = {Keys::Action::None, Keys::Action::None, Keys::Action::None};
        uint8_t       members = 0;        // Combo: how many of keys[]
        uint8_t       taps = 3;           // Sequence
        uint16_t      windowMs = 600;     // Sequence: between taps
        bool          swallow = true;     // keep the presses from the sinks
        bool valid() const;               // Gesture::valid's trigger half
    };

    struct RuleRow {
        static constexpr uint8_t  MAX_ACTIONS = 3;
        // A signal rule fires at most this often: an edge rule on a moving
        // value would otherwise keep the glass on popups for the drive. A
        // gesture has its own debounce (it re-arms on release): any pause, 0 too.
        static constexpr uint16_t PAUSE_MIN_S = 5;
        // An event rule (its formula uses changed(), rises() or falls()) is
        // true for one evaluation, so it cannot hold the glass: two gear
        // shifts 2 s apart are two popups. 1 s still keeps a noisy signal
        // from firing at the bus rate.
        static constexpr uint16_t PAUSE_MIN_EVENT_S = 1;
        static constexpr uint16_t pauseMin(bool event) { return event ? PAUSE_MIN_EVENT_S : PAUSE_MIN_S; }

        char        name[RULE_NAME + 1] = "";
        bool        on = true;
        RuleGesture gesture;
        // The formula: a signal rule's trigger; a gesture's "only if" ("" = always).
        char        when[Expr::Program::MAX_SRC + 1] = "";
        uint16_t    pauseS = PAUSE_MIN_S;
        RuleAction  act[MAX_ACTIONS];
        uint8_t     nAct = 0;

        bool isGesture() const { return gesture.trigger != Keys::Trigger::Off; }
    };

    struct RuleSet {
        static constexpr uint16_t MAX_ROWS = 128;
        PsArray<RuleRow> rows;
        // Which save this is (runtime only, 0 = boot's): a gesture carries it
        // with its row index, so one seen between the keys task's new table
        // and the car task's new set cannot fire another row.
        uint16_t gen = 0;
        // The three alerts 1.6 shipped: the cluster's low-fuel lamp, driving
        // without lights, driving with the handbrake up. Names the apps know.
        void factory();
    };

    // What is wrong with one row: its index and the e.<code> (rule_name,
    // rule_when, rule_pause, rule_action, rule_gesture, rule_rows, or an
    // Expr error with the byte in the formula).
    struct RuleProblem {
        uint16_t    row = 0;
        Expr::Error e;
    };

    class Rules {
    public:
        Rules() = default;
        ~Rules();
        Rules(const Rules&) = delete;
        Rules& operator=(const Rules&) = delete;

        // Every row sound: a name, a formula that compiles (a signal rule must
        // have one), a pause of pauseMin() or more for a signal rule, at
        // least one action and each with what it needs, a gesture that
        // Gesture::valid would take. False with the first problem.
        static bool check(const RuleSet& set, RuleProblem& p);

        // The car task: compiles every row and starts their state afresh. Rows
        // that do not check are off (the problem says which). False when one
        // did not. The rows' objects are allocated once (PSRAM) and reused:
        // a reader on another task (live, holds) never meets a freed one.
        bool apply(const RuleSet& set, RuleProblem& p);

        // After a pass's formulas: re-evaluates the rows whose inputs are in
        // `changed` (and, with `tick`, every timed row). A signal rule whose
        // formula went false -> true, with its pause run out, fires: `fire`
        // is called with its row, in row order. A gesture rule's formula only
        // updates armed().
        using FireFn = void (*)(uint16_t row, void* ctx);
        void step(const State& st, const Expr::SigSet& changed, uint32_t nowMs, bool tick, FireFn fire, void* ctx);

        // A gesture rule the keys task detected: true when it may fire now
        // (on, its "only if" holds, its pause has run out); counts it.
        bool gestureFire(uint16_t row, uint32_t nowMs);

        // The row's formula at its last evaluation ("" holds). For a gesture
        // rule: whether the gesture may fire now. For a pending popup: whether
        // the rule still holds. Any task (plain reads of a row that stays put).
        bool holds(uint16_t row) const;
        bool armed(uint16_t row) const { return holds(row); }
        // The row's formula is an event (Expr::Program::pulsed): its popup is
        // owed once it fired, whatever the formula says a pass later.
        bool event(uint16_t row) const;

        // The signals the row's formula reads (empty for a formula-less gesture).
        const Expr::SigSet& inputsOf(uint16_t row) const;

        struct Live {
            bool     on = false, gesture = false, holds = false, running = false, event = false;
            uint32_t fires = 0, lastFireMs = 0;
        };
        bool     live(uint16_t row, Live& out) const;   // any task
        uint16_t rows() const { return _n; }

        // The popup text of an action: `arg` with every {signal} replaced by
        // that signal's value (its decimal point placed, no unit), or, with
        // `arg` empty, "name: value unit" of the formula's first input, plus
        // " ?" when that signal is not verified; a rule with no input gives
        // its name. The length written.
        static int popupText(char* b, size_t cap, const RuleRow& r, const char* arg,
                             const Expr::SigSet& inputs, const State& st);
        // The value the apps get with a popup: the formula's first input, as
        // State holds it; 0 when there is none.
        static int32_t popupValue(const RuleRow& r, const Expr::SigSet& inputs, const State& st);

        // The gesture rows of a set as the keys task's GestureTable: macro i
        // stands for rule `rule` (Keys::Gesture::rule), its effect the rule's
        // first action (Gesture::valid wants one; the car task performs them
        // all). How many, at most `cap` (GestureTable::COUNT); the rest of
        // `out` is cleared.
        static uint8_t gestures(const RuleSet& set, Keys::GestureTable& out, uint8_t cap);


    private:
        struct Row {
            Expr::Program        prog;      // empty for a formula-less gesture
            Expr::Program::State st;
            bool     on = false, gesture = false;
            bool     holds = false;         // the last evaluation ("" = true)
            bool     seen = false;          // evaluated at least once
            bool     quiet = false;         // a set saved while running: the first look only learns the level
            uint32_t fires = 0, lastFireMs = 0;
            uint16_t pauseS = 0;
        };
        bool rowsReady();
        // MAX_ROWS Rows in one block (PSRAM where there is one), made on the
        // first apply and kept: Program and State own PsArrays, so a Row is
        // not a plain record, and a reader on another task must never find
        // the row it reads gone.
        Row*     _rows = nullptr;
        uint16_t _n = 0;
        bool     _applied = false;          // a set has been applied before (boot's)
        Expr::SigSet _none;
    };
}
