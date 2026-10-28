#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../utils/PsArray.h"

// The formula language (docs/design/rules.md, slice 2): one small language for
// user signals (`fuel_level * 100 / 60`), rule conditions (`speed > 10 &&
// !lights_dipped`) and head-unit bindings. Typed on the board's page, so it is
// parsed on the board: once, on save, into a bytecode a stack machine runs
// whenever an input changed (inputs()) or, for the functions of time, on a
// tick (timed()).
//
// NUMBERS are fixed point, int64 x 1000 (Num): a signal is read in its own
// units, whatever its decimals (speed is km/h x100 in State; in a formula
// `speed > 10` is 10 km/h). No floats on the hot path.
//
// NO VALUE (a signal never seen, a division by 0) is NONE. It carries through
// arithmetic and comparisons; `a && b` is 0 when either is 0, `a || b` is 1
// when either is 1, NONE otherwise. A condition that is NONE does not hold;
// a user signal that is NONE has no value. `has(x)` asks.
//
//   or     := and  { ("||" | "or")  and }
//   and    := cmp  { ("&&" | "and") cmp }
//   cmp    := add  [ ("<" | "<=" | ">" | ">=" | "==" | "!=") add ]
//   add    := mul  { ("+" | "-") mul }
//   mul    := un   { ("*" | "/" | "%") un }
//   un     := ("-" | "!" | "not") un | prim
//   prim   := number | name | name "(" [ or { "," or } ] ")" | "(" or ")"
//
// Functions. Pure: min(a, b, ...), max(...), abs(x), clamp(x, lo, hi),
// if(c, a, b), has(x), round(x), lookup(x, x1, y1, x2, y2, ...) (straight
// lines between the points, flat past the ends), age(name) (s since the
// signal was last received; 0 while it is pinned).
// With state (each call keeps its own): delta(x) (x minus x at the last
// evaluation), changed(x), rises(c), falls(c) (1 on the evaluation where it
// happens), rate(x) (per second), integral(x) (x times seconds, summed),
// avg(x, n) (a moving average: exponential, time constant n s; 63 % of a step
// after n s, 95 % after 3n),
// held(c, n) (1 once c has held for n s). n must be a constant.
//
// Portable: env:native tests it (test_expr).
namespace Expr
{
    using Num = int64_t;
    constexpr Num SCALE = 1000;
    constexpr Num NONE  = INT64_MIN;

    // Every signal a formula reads, one bit each (ids up to 255).
    struct SigSet {
        uint32_t w[8] = {};
        void set(uint8_t id) { w[id >> 5] |= 1u << (id & 31); }
        bool has(uint8_t id) const { return (w[id >> 5] >> (id & 31)) & 1u; }
        bool any(const SigSet& o) const
        {
            for (int i = 0; i < 8; ++i) if (w[i] & o.w[i]) return true;
            return false;
        }
        void clear() { for (uint32_t& x : w) x = 0; }
    };

    // A name in a formula to a signal id; -1: no such signal.
    using ResolveFn = int (*)(const char* name, size_t len, void* ctx);
    // A signal's value in its own units, x SCALE, and its age; false: none.
    using ReadFn = bool (*)(uint8_t id, Num& value, uint32_t& ageMs, void* ctx);

    struct Error {
        const char* code = nullptr;   // an e.<code>: expr_syntax, expr_name, ...
        uint16_t    pos  = 0;         // the byte where it was found
    };

    struct Op {
        uint8_t  code;
        uint8_t  argc;
        uint16_t slot;   // a stateful call's state, or the signal id
        Num      k;      // a constant, or a function's constant argument
    };

    class Program {
    public:
        // Sanity caps: a formula a person types, not a program.
        static constexpr uint16_t MAX_SRC   = 255;
        static constexpr uint16_t MAX_OPS   = 128;
        static constexpr uint8_t  MAX_STACK = 16;

        // Replaces this program; false with `err` set and the program empty.
        bool compile(const char* src, ResolveFn resolve, void* ctx, Error& err);
        void clear();

        bool empty() const { return _ops.empty(); }
        const SigSet& inputs() const { return _in; }
        // It reads the clock (held, avg, rate, integral): evaluate on a tick
        // too, not only when an input changed.
        bool timed() const { return _timed; }
        uint16_t stateSlots() const { return _slots; }

        // State for one program's stateful calls; reset() when the program
        // is compiled again.
        struct State {
            struct Slot { Num a, b; uint32_t t; uint8_t seen; };
            PsArray<Slot> s;
        };
        bool reset(State& st) const { return st.s.resize(_slots); }

        // NONE when it has no value.
        Num eval(State& st, ReadFn read, void* ctx, uint32_t nowMs) const;

        const PsArray<Op>& ops() const { return _ops; }

    private:
        PsArray<Op> _ops;
        SigSet      _in;
        bool        _timed = false;
        uint16_t    _slots = 0;
        friend struct Parser;
    };

    // A word the language takes (true, false, and, or, not, a function's
    // name): no signal may be called that.
    bool reserved(const char* name, size_t len);

    // A formula's value as a condition: NONE and 0 do not hold.
    inline bool holds(Num v) { return v != NONE && v != 0; }
    // To and from a signal's own integer (decimals `dec`), rounded.
    Num     fromRaw(int32_t raw, uint8_t dec);
    bool    toRaw(Num v, uint8_t dec, int32_t& raw);   // false: NONE or out of int32
}
