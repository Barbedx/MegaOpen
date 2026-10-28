#include "Expr.h"
#include <math.h>
#include <string.h>

namespace Expr
{
    namespace
    {
        enum Code : uint8_t {
            PUSH = 1, SIG,
            ADD, SUB, MUL, DIV, MOD, NEG, NOT,
            LT, LE, GT, GE, EQ, NE, AND, OR,
            // pure functions
            F_MIN, F_MAX, F_ABS, F_CLAMP, F_IF, F_HAS, F_ROUND, F_LOOKUP, F_AGE,
            // with state
            F_DELTA, F_CHANGED, F_RISES, F_FALLS, F_RATE, F_INTEGRAL, F_AVG, F_HELD,
        };

        // A function: its op, how many arguments (min..max; 0xFF any), whether
        // it keeps state, reads the clock, and takes a constant as its last
        // argument.
        struct Fn {
            const char* name;
            uint8_t     code, minArgs, maxArgs;
            bool        state, timed, constLast;
        };
        const Fn FNS[] = {
            {"min", F_MIN, 2, 0xFF, false, false, false},
            {"max", F_MAX, 2, 0xFF, false, false, false},
            {"abs", F_ABS, 1, 1, false, false, false},
            {"clamp", F_CLAMP, 3, 3, false, false, false},
            {"if", F_IF, 3, 3, false, false, false},
            {"has", F_HAS, 1, 1, false, false, false},
            {"round", F_ROUND, 1, 1, false, false, false},
            {"lookup", F_LOOKUP, 5, 0xFF, false, false, false},
            {"age", F_AGE, 1, 1, false, true, false},
            {"delta", F_DELTA, 1, 1, true, false, false},
            {"changed", F_CHANGED, 1, 1, true, false, false},
            {"rises", F_RISES, 1, 1, true, false, false},
            {"falls", F_FALLS, 1, 1, true, false, false},
            {"rate", F_RATE, 1, 1, true, true, false},
            {"integral", F_INTEGRAL, 1, 1, true, true, false},
            {"avg", F_AVG, 2, 2, true, true, true},
            {"held", F_HELD, 2, 2, true, true, true},
        };

        bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
        bool isDigit(char c) { return c >= '0' && c <= '9'; }
        bool word(const char* s, size_t n, const char* w) { return strlen(w) == n && !strncmp(s, w, n); }

        Num bin(bool v) { return v ? SCALE : 0; }

        // a * b / SCALE, NONE on overflow.
        Num mulFix(Num a, Num b)
        {
            Num p;
            if (!__builtin_mul_overflow(a, b, &p)) return p / SCALE;
            Num q;
            if (__builtin_mul_overflow(a / SCALE, b, &q)) return NONE;
            return q;
        }
        // a * SCALE / b, NONE on overflow or b == 0.
        Num divFix(Num a, Num b)
        {
            if (!b) return NONE;
            Num p;
            if (!__builtin_mul_overflow(a, SCALE, &p)) return p / b;
            return a / b * SCALE;
        }
        // a * b / c, NONE on overflow or c == 0.
        Num muldiv(Num a, Num b, Num c)
        {
            if (!c) return NONE;
            Num p;
            if (__builtin_mul_overflow(a, b, &p)) return NONE;
            return p / c;
        }
    }

    struct Parser {
        const char* s;
        size_t      i = 0;
        Program&    p;
        ResolveFn   resolve;
        void*       ctx;
        Error&      err;
        int         depth = 0, maxDepth = 0;

        bool fail(const char* code, size_t at)
        {
            if (!err.code) { err.code = code; err.pos = (uint16_t)at; }
            return false;
        }
        void ws() { while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') ++i; }
        bool eat(const char* t)
        {
            ws();
            const size_t n = strlen(t);
            if (strncmp(s + i, t, n)) return false;
            i += n;
            return true;
        }
        // A keyword operator: not the start of a longer name.
        bool eatWord(const char* w)
        {
            ws();
            const size_t n = strlen(w);
            if (strncmp(s + i, w, n) || isAlpha(s[i + n]) || isDigit(s[i + n])) return false;
            i += n;
            return true;
        }
        bool emit(uint8_t code, int stack, uint8_t argc = 0, uint16_t slot = 0, Num k = 0)
        {
            if (p._ops.size() >= Program::MAX_OPS) return fail("expr_long", i);
            if (!p._ops.push(Op{code, argc, slot, k})) return fail("no_memory", i);
            depth += stack;
            if (depth > maxDepth) maxDepth = depth;
            if (maxDepth > Program::MAX_STACK) return fail("expr_deep", i);
            return true;
        }

        bool orExpr()
        {
            if (!andExpr()) return false;
            for (;;) {
                if (eat("||") || eatWord("or")) { if (!andExpr() || !emit(OR, -1)) return false; }
                else return true;
            }
        }
        bool andExpr()
        {
            if (!cmpExpr()) return false;
            for (;;) {
                if (eat("&&") || eatWord("and")) { if (!cmpExpr() || !emit(AND, -1)) return false; }
                else return true;
            }
        }
        bool cmpExpr()
        {
            if (!addExpr()) return false;
            uint8_t op = 0;
            if      (eat("<=")) op = LE;
            else if (eat(">=")) op = GE;
            else if (eat("==")) op = EQ;
            else if (eat("!=")) op = NE;
            else if (eat("<"))  op = LT;
            else if (eat(">"))  op = GT;
            if (!op) return true;
            return addExpr() && emit(op, -1);
        }
        bool addExpr()
        {
            if (!mulExpr()) return false;
            for (;;) {
                if (eat("+"))      { if (!mulExpr() || !emit(ADD, -1)) return false; }
                else if (eat("-")) { if (!mulExpr() || !emit(SUB, -1)) return false; }
                else return true;
            }
        }
        bool mulExpr()
        {
            if (!unary()) return false;
            for (;;) {
                if (eat("*"))      { if (!unary() || !emit(MUL, -1)) return false; }
                else if (eat("/")) { if (!unary() || !emit(DIV, -1)) return false; }
                else if (eat("%")) { if (!unary() || !emit(MOD, -1)) return false; }
                else return true;
            }
        }
        bool unary()
        {
            ws();
            // "!=" is not a not.
            if (s[i] == '!' && s[i + 1] != '=') { ++i; return unary() && emit(NOT, 0); }
            if (eatWord("not")) return unary() && emit(NOT, 0);
            if (eat("-")) {
                // A negative literal folds into it.
                const uint16_t at = p._ops.size();
                if (!unary()) return false;
                if (p._ops.size() == at + 1 && p._ops[at].code == PUSH) { p._ops[at].k = -p._ops[at].k; return true; }
                return emit(NEG, 0);
            }
            return primary();
        }
        bool number()
        {
            const size_t at = i;
            Num v = 0;
            while (isDigit(s[i])) {
                if (v > (INT64_MAX / SCALE - 9) / 10) return fail("expr_range", at);
                v = v * 10 + (s[i++] - '0');
            }
            v *= SCALE;
            if (s[i] == '.') {
                ++i;
                if (!isDigit(s[i])) return fail("expr_syntax", i);
                Num f = 0, w = SCALE / 10;
                bool up = false;
                for (int d = 0; isDigit(s[i]); ++d, ++i) {
                    if (d < 3) { f += (s[i] - '0') * w; w /= 10; }
                    else if (d == 3) up = s[i] >= '5';   // the 4th digit rounds
                }
                v += f + (up ? 1 : 0);
            }
            if (isAlpha(s[i])) return fail("expr_syntax", i);
            return emit(PUSH, 1, 0, 0, v);
        }
        bool primary()
        {
            ws();
            const size_t at = i;
            if (isDigit(s[i])) return number();
            if (eat("(")) {
                if (!orExpr()) return false;
                if (!eat(")")) return fail("expr_paren", i);
                return true;
            }
            if (!isAlpha(s[i])) return fail(s[i] ? "expr_syntax" : "expr_end", i);
            while (isAlpha(s[i]) || isDigit(s[i])) ++i;
            const size_t n = i - at;
            ws();
            if (s[i] == '(') { ++i; return call(s + at, n, at); }
            // true / false, then a signal.
            if (word(s + at, n, "true"))  return emit(PUSH, 1, 0, 0, SCALE);
            if (word(s + at, n, "false")) return emit(PUSH, 1, 0, 0, 0);
            const int id = resolve ? resolve(s + at, n, ctx) : -1;
            if (id < 0 || id > 255) return fail("expr_name", at);
            p._in.set((uint8_t)id);
            return emit(SIG, 1, 0, (uint16_t)id);
        }
        bool call(const char* name, size_t n, size_t at)
        {
            const Fn* f = nullptr;
            for (const Fn& x : FNS) if (word(name, n, x.name)) f = &x;
            if (!f) return fail("expr_func", at);
            uint8_t argc = 0;
            uint16_t lastAt = p._ops.size();
            ws();
            if (!eat(")")) {
                for (;;) {
                    lastAt = p._ops.size();
                    if (!orExpr()) return false;
                    if (++argc > 32) return fail("expr_args", at);
                    if (eat(",")) continue;
                    if (eat(")")) break;
                    return fail("expr_paren", i);
                }
            }
            if (argc < f->minArgs || (f->maxArgs != 0xFF && argc > f->maxArgs)) return fail("expr_args", at);
            if (f->code == F_LOOKUP && !(argc & 1)) return fail("expr_args", at);   // x, then pairs
            Num k = 0;
            if (f->constLast) {
                // The last argument is a constant (a time in s): taken out of
                // the program, kept with the call.
                if (p._ops.size() != lastAt + 1 || p._ops[lastAt].code != PUSH) return fail("expr_const", at);
                k = p._ops[lastAt].k;
                if (k <= 0) return fail("expr_const", at);
                p._ops.erase(lastAt);
                --depth;
                --argc;
            }
            uint16_t slot = 0;
            if (f->code == F_AGE) {
                // age(name): the signal itself, not its value.
                if (p._ops.size() != lastAt + 1 || p._ops[lastAt].code != SIG) return fail("expr_name", at);
                slot = p._ops[lastAt].slot;
                p._ops.erase(lastAt);
                --depth;
                argc = 0;
            } else if (f->state) {
                slot = p._slots++;
            }
            if (f->timed) p._timed = true;
            return emit(f->code, 1 - (int)argc, argc, slot, k);
        }
    };

    void Program::clear()
    {
        _ops.clear();
        _in.clear();
        _timed = false;
        _slots = 0;
    }

    bool Program::compile(const char* src, ResolveFn resolve, void* ctx, Error& err)
    {
        clear();
        err = Error{};
        if (!src) src = "";
        if (strlen(src) > MAX_SRC) { err.code = "expr_long"; err.pos = MAX_SRC; return false; }
        Parser ps{src, 0, *this, resolve, ctx, err};
        ps.ws();
        if (!src[ps.i]) { err.code = "expr_empty"; return false; }
        bool ok = ps.orExpr();
        ps.ws();
        if (ok && src[ps.i]) ok = ps.fail("expr_syntax", ps.i);
        if (!ok) { clear(); return false; }
        return true;
    }

    Num fromRaw(int32_t raw, uint8_t dec)
    {
        Num v = (Num)raw * SCALE;
        for (uint8_t d = 0; d < dec; ++d) v /= 10;
        return v;
    }

    bool toRaw(Num v, uint8_t dec, int32_t& raw)
    {
        if (v == NONE) return false;
        Num p = 1;
        for (uint8_t d = 0; d < dec; ++d) p *= 10;
        Num m;
        if (__builtin_mul_overflow(v, p, &m)) return false;
        const Num r = (m >= 0 ? m + SCALE / 2 : m - SCALE / 2) / SCALE;   // to the nearest
        if (r < INT32_MIN || r > INT32_MAX) return false;
        raw = (int32_t)r;
        return true;
    }

    Num Program::eval(State& st, ReadFn read, void* ctx, uint32_t now) const
    {
        if (_ops.empty() || st.s.size() < _slots) return NONE;
        Num stk[MAX_STACK];
        int sp = 0;
        auto truthy = [](Num v) { return v != NONE && v != 0; };
        for (uint16_t pc = 0; pc < _ops.size(); ++pc) {
            const Op& o = _ops[pc];
            switch (o.code) {
                case PUSH: stk[sp++] = o.k; break;
                case SIG: {
                    Num v = NONE;
                    uint32_t age = 0;
                    if (!read || !read((uint8_t)o.slot, v, age, ctx)) v = NONE;
                    stk[sp++] = v;
                    break;
                }
                case NEG: if (stk[sp - 1] != NONE) stk[sp - 1] = -stk[sp - 1]; break;
                case NOT: if (stk[sp - 1] != NONE) stk[sp - 1] = bin(stk[sp - 1] == 0); break;
                case AND: {
                    const Num b = stk[--sp], a = stk[sp - 1];
                    stk[sp - 1] = (a == 0 || b == 0) ? 0 : (a == NONE || b == NONE) ? NONE : SCALE;
                    break;
                }
                case OR: {
                    const Num b = stk[--sp], a = stk[sp - 1];
                    stk[sp - 1] = (truthy(a) || truthy(b)) ? SCALE : (a == NONE || b == NONE) ? NONE : 0;
                    break;
                }
                case ADD: case SUB: case MUL: case DIV: case MOD:
                case LT: case LE: case GT: case GE: case EQ: case NE: {
                    const Num b = stk[--sp], a = stk[sp - 1];
                    Num r = NONE;
                    if (a != NONE && b != NONE) {
                        switch (o.code) {
                            case ADD: if (__builtin_add_overflow(a, b, &r)) r = NONE; break;
                            case SUB: if (__builtin_sub_overflow(a, b, &r)) r = NONE; break;
                            case MUL: r = mulFix(a, b); break;
                            case DIV: r = divFix(a, b); break;
                            case MOD: r = b ? a % b : NONE; break;
                            case LT: r = bin(a < b); break;
                            case LE: r = bin(a <= b); break;
                            case GT: r = bin(a > b); break;
                            case GE: r = bin(a >= b); break;
                            case EQ: r = bin(a == b); break;
                            case NE: r = bin(a != b); break;
                        }
                    }
                    stk[sp - 1] = r;
                    break;
                }
                case F_MIN: case F_MAX: {
                    sp -= o.argc;
                    Num r = NONE;
                    for (uint8_t a = 0; a < o.argc; ++a) {
                        const Num v = stk[sp + a];
                        if (v == NONE) { r = NONE; break; }
                        if (a == 0 || (o.code == F_MIN ? v < r : v > r)) r = v;
                    }
                    stk[sp++] = r;
                    break;
                }
                case F_ABS: if (stk[sp - 1] != NONE && stk[sp - 1] < 0) stk[sp - 1] = -stk[sp - 1]; break;
                case F_ROUND: {
                    Num& v = stk[sp - 1];
                    if (v != NONE) v = (v >= 0 ? v + SCALE / 2 : v - SCALE / 2) / SCALE * SCALE;
                    break;
                }
                case F_HAS: stk[sp - 1] = bin(stk[sp - 1] != NONE); break;
                case F_CLAMP: {
                    const Num hi = stk[--sp], lo = stk[--sp], x = stk[sp - 1];
                    stk[sp - 1] = (x == NONE || lo == NONE || hi == NONE) ? NONE : x < lo ? lo : x > hi ? hi : x;
                    break;
                }
                case F_IF: {
                    const Num b = stk[--sp], a = stk[--sp], c = stk[sp - 1];
                    stk[sp - 1] = c == NONE ? NONE : c ? a : b;
                    break;
                }
                case F_LOOKUP: {
                    sp -= o.argc;
                    const Num* a = stk + sp;
                    const Num x = a[0];
                    const uint8_t pts = (uint8_t)((o.argc - 1) / 2);
                    Num r = NONE;
                    bool bad = x == NONE;
                    for (uint8_t q = 1; q < o.argc; ++q) bad |= a[q] == NONE;
                    if (!bad) {
                        if (x <= a[1]) r = a[2];
                        else if (x >= a[2 * pts - 1]) r = a[2 * pts];
                        else
                            for (uint8_t q = 0; q + 1 < pts; ++q) {
                                const Num x0 = a[1 + 2 * q], y0 = a[2 + 2 * q], x1 = a[3 + 2 * q], y1 = a[4 + 2 * q];
                                if (x < x0 || x > x1) continue;
                                const Num d = muldiv(x - x0, y1 - y0, x1 - x0);
                                r = d == NONE ? NONE : y0 + d;
                                break;
                            }
                    }
                    stk[sp++] = r;
                    break;
                }
                case F_AGE: {
                    Num v = NONE;
                    uint32_t age = 0;
                    stk[sp++] = read && read((uint8_t)o.slot, v, age, ctx) ? (Num)age : NONE;   // ms = s x SCALE
                    break;
                }
                default: {
                    // With state. Slot: a = the last input (or the running
                    // value), b = the last output, t = its time, seen bit 0 =
                    // an input was seen, bit 1 = held started.
                    State::Slot& z = st.s[o.slot];
                    const Num x = stk[sp - 1];
                    Num r = NONE;
                    const uint32_t dt = (uint32_t)(now - z.t);
                    switch (o.code) {
                        case F_DELTA:
                            if (x == NONE) break;
                            r = (z.seen & 1) ? x - z.a : 0;
                            z.a = x; z.seen |= 1;
                            break;
                        case F_CHANGED:
                            if (x == NONE) break;
                            r = bin((z.seen & 1) && x != z.a);
                            z.a = x; z.seen |= 1;
                            break;
                        case F_RISES: case F_FALLS: {
                            const bool on = truthy(x);
                            const bool was = (z.seen & 1) && z.a;
                            r = bin((z.seen & 1) && (o.code == F_RISES ? on && !was : !on && was));
                            z.a = on; z.seen |= 1;
                            break;
                        }
                        case F_RATE:
                            if (x == NONE) break;
                            if (!(z.seen & 1)) { z.a = x; z.t = now; z.b = NONE; z.seen |= 1; break; }
                            if (dt) { z.b = muldiv(x - z.a, 1000, dt); z.a = x; z.t = now; }
                            r = z.b;
                            break;
                        case F_INTEGRAL:
                            if (!(z.seen & 1)) { z.b = 0; z.t = now; z.seen |= 1; r = 0; break; }
                            if (x != NONE && dt) {
                                const Num d = muldiv(x, dt, 1000);
                                if (d != NONE) z.b += d;
                            }
                            z.t = now;
                            r = z.b;
                            break;
                        case F_AVG:
                            if (x == NONE) { r = (z.seen & 1) ? z.a : NONE; break; }
                            if (!(z.seen & 1)) { z.a = x; z.t = now; z.seen |= 1; r = x; break; }
                            if (dt) {
                                // Exponential with time constant tau (o.k: s x SCALE = ms),
                                // exact for any step: a += (x - a) * (1 - e^(-dt / tau)).
                                const double f = 1.0 - exp(-(double)dt / (double)o.k);
                                z.a += (Num)llround((double)(x - z.a) * f);
                                z.t = now;
                            }
                            r = z.a;
                            break;
                        case F_HELD:
                            if (!truthy(x)) { z.seen &= (uint8_t)~2; r = 0; break; }
                            if (!(z.seen & 2)) { z.seen |= 2; z.t = now; }
                            r = bin((Num)(uint32_t)(now - z.t) >= o.k);   // o.k: s x SCALE = ms
                            break;
                    }
                    stk[sp - 1] = r;
                    break;
                }
            }
        }
        return sp == 1 ? stk[0] : NONE;
    }
}
