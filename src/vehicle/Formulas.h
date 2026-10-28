#pragma once
#include <stdint.h>
#include "Expr.h"
#include "VehicleState.h"
#include "../utils/PsArray.h"

// Signals computed by formulas (docs/design/rules.md, slice 3): the owner's own
// signals and the factory's computed ones (fuel_pct, range_calc), one row each,
// edited on the page and kept in the car file.
//
// A row's target is a built-in signal (its formula feeds it: the factory rows
// do this) or a new name, which takes a free user slot (SIG_USER_*) with the
// row's unit, decimals and yes/no. A target bound to CAN or a constant on the
// map is fed by that instead: the row stands down, as Derived's math does.
//
// The car task runs it after each pass's decode: a row whose inputs changed
// (State::takeChanged) is evaluated again, and every row with a function of
// time on each tick. Rows run in their order, and a row's new value counts as
// a change for the rows after it in the same pass.
//
// Portable: env:native tests it (test_formulas).
namespace Vehicle
{
    struct FormulaRow {
        char    name[USER_NAME + 1] = "";
        char    src[Expr::Program::MAX_SRC + 1] = "";
        char    unit[USER_UNIT + 1] = "";     // a new signal's; a built-in has its own
        uint8_t decimals = 0;                 // likewise (0..3)
        bool    boolean = false;              // likewise
        bool    on = true;
    };

    struct FormulaSet {
        // Built-ins the factory computes, and the owner's own.
        static constexpr uint16_t MAX_ROWS = 128;
        PsArray<FormulaRow> rows;
        // fuel_pct and range_calc: what Derived computed in C++ up to 1.6.
        void factory();
    };

    // What is wrong with one row: its index and the e.<code> (an Expr error, or
    // formula_name, formula_twice, formula_rows), with the byte for a formula's.
    struct FormulaProblem {
        uint16_t    row = 0;
        Expr::Error e;
    };

    class Formulas {
    public:
        Formulas() = default;
        ~Formulas() { for (Live*& l : _row) { delete l; l = nullptr; } }
        Formulas(const Formulas&) = delete;
        Formulas& operator=(const Formulas&) = delete;

        // The names a set would give, checked before anything is applied (the
        // web, a settings file): a target is a built-in signal or a new
        // [a-z_][a-z0-9_]* name of at most USER_NAME characters, no word the
        // language takes (true, and, a function's), once each; at most
        // USER_SIGNALS new ones; every formula compiles, its names being the
        // built-ins and this set's own. False with the first problem.
        static bool check(const FormulaSet& set, FormulaProblem& p);

        // The car task: names the user slots (a name that was there keeps its
        // slot), compiles every row and starts their state afresh. Rows that do
        // not check are off (the problem says which). False when one did not.
        bool apply(const FormulaSet& set, FormulaProblem& p);

        // After a pass's decode: re-evaluates the rows whose inputs are in
        // `changed` (and, with `tick`, every timed row), writes their targets
        // and adds the targets that changed to `changed`.
        void step(State& st, Expr::SigSet& changed, uint32_t nowMs, bool tick);

        // The targets the rows feed (Derived stands down for them), and a
        // target's inputs (for Vehicle::verified: as good as its weakest).
        const Expr::SigSet& targets() const { return _targets; }
        bool inputsOf(uint8_t sig, Expr::SigSet& out) const;

        // A target that something else feeds (a CAN row, a constant): its
        // row stands down. The car task sets these from the decode table.
        void setBound(const Expr::SigSet& b) { _bound = b; }

        uint16_t rows() const { return _live.size(); }

    private:
        struct Live {
            uint8_t               target;
            Expr::Program         prog;
            Expr::Program::State  st;
        };
        // Program and State own PSRAM arrays: Live is not a plain record, so
        // the rows are a fixed array of pointers to heap objects.
        Live*        _row[FormulaSet::MAX_ROWS] = {};
        PsArray<uint8_t> _live;          // indexes into _row of the rows that run
        Expr::SigSet _targets, _bound;
        // Which name each user slot had: a name keeps its slot. In PSRAM,
        // like apply()'s working space (the car task's stack is small).
        struct Name { char n[USER_NAME + 1]; };
        PsArray<Name> _slotName;
    };
}
