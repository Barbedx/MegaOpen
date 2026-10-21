#include "MenuEngine.h"
#include <stdio.h>
#include <string.h>

namespace MenuUi
{
    using namespace Cfg;

    namespace
    {
        constexpr uint8_t ROOT = 0xFF;
        constexpr uint8_t TEXT = COLS - 1;      // column 0 is the cursor marker

        // `left` from column 0, `right` flush with the end; `right` wins the
        // space when both do not fit.
        void lay(char* out, size_t cap, const char* left, const char* right, size_t width = TEXT)
        {
            const size_t w = cap - 1 < width ? cap - 1 : width;
            const size_t rl = right ? strlen(right) : 0;
            const size_t r = rl < w ? rl : w;
            const size_t room = w - r;
            size_t l = left ? strlen(left) : 0;
            if (l > room) l = room;
            memset(out, ' ', w);
            if (l) memcpy(out, left, l);
            if (r) memcpy(out + (w - r), right, r);
            // Trailing blanks only cost wire bytes.
            size_t end = w;
            while (end > 0 && out[end - 1] == ' ') --end;
            out[end] = 0;
        }
    }

    void Engine::begin(const Model* m, const CarView* car)
    {
        _m = m;
        _car = car;
        _open = false;
    }

    void Engine::open()
    {
        _open = true;
        _editing = false;
        _depth = 0;
        _stack[0] = Level{Kind::Group, ROOT, 0};
        build();
        _cursor = _n > 1 ? 1 : 0;      // past "< Exit"
    }

    void Engine::close()
    {
        _open = false;
        _editing = false;
    }

    bool Engine::groupHasItems(uint8_t regIndex) const
    {
        const SEntry& g = Registry::at(regIndex);
        for (size_t i = 0; i < Registry::count(); ++i) {
            const SEntry& e = Registry::at(i);
            if (!e.parent || strcmp(e.parent, g.id) || !(e.flags & F_MENU)) continue;
            if (e.type == SType::Group ? groupHasItems((uint8_t)i) : Registry::available(*_m, e))
                return true;
        }
        return false;
    }

    void Engine::build()
    {
        _n = 0;
        const Level& lv = _stack[_depth];
        auto add = [&](Kind k, uint8_t ref) { if (_n < MAX_ITEMS) _items[_n++] = Item{k, ref}; };

        // Every level opens with a way out: "< Back", or at the root "< Exit"
        // (the hold that opened the menu also closes it, but nobody has to know).
        add(_depth > 0 ? Kind::Back : Kind::Exit, 0);

        switch (lv.kind) {
            case Kind::Group: {
                const char* parent = lv.ref == ROOT ? nullptr : Registry::at(lv.ref).id;
                for (size_t i = 0; i < Registry::count() && i < ROOT; ++i) {
                    const SEntry& e = Registry::at(i);
                    if (!(e.flags & F_MENU)) continue;
                    if (parent ? (!e.parent || strcmp(e.parent, parent)) : e.parent != nullptr) continue;
                    if (e.type == SType::Group) {
                        if (groupHasItems((uint8_t)i)) add(Kind::Group, (uint8_t)i);
                    } else if (_m && Registry::available(*_m, e)) {
                        add(Kind::Setting, (uint8_t)i);
                    }
                }
                if (lv.ref == ROOT) {
                    if (_car && _car->groups) add(Kind::CarRoot, 0);
                    add(Kind::PowerOff, 0);
                }
                break;
            }
            case Kind::CarRoot: {
                uint8_t sig[1];
                const uint8_t n = _car->groups(_car->ctx);
                for (uint8_t g = 0; g < n; ++g)
                    if (_car->signals(_car->ctx, g, sig, 1)) add(Kind::CarGroup, g);
                break;
            }
            case Kind::CarGroup: {
                uint8_t sig[MAX_ITEMS];
                const uint8_t n = _car->signals(_car->ctx, lv.ref, sig, MAX_ITEMS - 1);
                for (uint8_t i = 0; i < n; ++i) add(Kind::Signal, sig[i]);
                break;
            }
            default:
                break;
        }
    }

    void Engine::enter(Kind kind, uint8_t ref)
    {
        if (_depth + 1 >= MAX_DEPTH) return;
        _stack[_depth].cursor = _cursor;
        _stack[++_depth] = Level{kind, ref, 0};
        build();
        _cursor = 0;
    }

    void Engine::leave()
    {
        if (!_depth) return;
        --_depth;
        build();
        _cursor = _stack[_depth].cursor;
        if (_cursor >= _n) _cursor = _n ? _n - 1 : 0;
    }

    void Engine::refresh()
    {
        if (!_open) return;
        const bool had = _cursor < _n;
        const Item was = had ? _items[_cursor] : Item{Kind::Back, 0};
        build();
        if (had)
            for (uint8_t i = 0; i < _n; ++i)
                if (_items[i].kind == was.kind && _items[i].ref == was.ref) { _cursor = i; return; }
        if (_cursor >= _n) _cursor = _n ? _n - 1 : 0;
        _editing = false;
    }

    Out Engine::key(Cmd c)
    {
        if (!_open) return Out::None;
        if (c == Cmd::Close) { close(); return Out::Closed; }

        if (_editing) {
            const SEntry& e = Registry::at(_items[_cursor].ref);
            if (c == Cmd::Next || c == Cmd::Prev) {
                const int32_t v = Registry::stepped(e, _editValue, c == Cmd::Next ? +1 : -1);
                if (v == _editValue) return Out::None;
                _editValue = v;
                return Out::Redraw;
            }
            _editing = false;
            if (_editValue == Registry::get(*_m, e)) return Out::Redraw;
            _commitEntry = &e;
            _commitValue = _editValue;
            return Out::Commit;
        }

        if (!_n) return Out::None;
        if (c == Cmd::Next) { _cursor = (uint8_t)((_cursor + 1) % _n); return Out::Redraw; }
        if (c == Cmd::Prev) { _cursor = (uint8_t)(_cursor ? _cursor - 1 : _n - 1); return Out::Redraw; }

        const Item it = _items[_cursor];
        switch (it.kind) {
            case Kind::Back:     leave(); return Out::Redraw;
            case Kind::Exit:     close(); return Out::Closed;
            case Kind::Group:    enter(Kind::Group, it.ref); return Out::Redraw;
            case Kind::CarRoot:  enter(Kind::CarRoot, 0); return Out::Redraw;
            case Kind::CarGroup: enter(Kind::CarGroup, it.ref); return Out::Redraw;
            case Kind::Signal:   return Out::None;
            case Kind::PowerOff: close(); return Out::PowerOff;
            case Kind::Setting: {
                const SEntry& e = Registry::at(it.ref);
                if (e.flags & F_RO) return Out::None;
                const int32_t v = Registry::get(*_m, e);
                if (e.type == SType::Bool) {
                    _commitEntry = &e;
                    _commitValue = v ? 0 : 1;
                    return Out::Commit;
                }
                _editing = true;
                _editValue = v;
                return Out::Redraw;
            }
        }
        return Out::None;
    }

    void Engine::item(uint8_t i, char* out, size_t cap) const
    {
        if (!cap) return;
        out[0] = 0;
        if (i >= _n) return;
        const Item& it = _items[i];
        switch (it.kind) {
            case Kind::Back:     lay(out, cap, "< Back", nullptr); break;
            case Kind::Exit:     lay(out, cap, "< Exit", nullptr); break;
            case Kind::Group:    lay(out, cap, Registry::at(it.ref).tag, ">"); break;
            case Kind::CarRoot:  lay(out, cap, "Car data", ">"); break;
            case Kind::CarGroup: lay(out, cap, _car->groupTag(_car->ctx, it.ref), ">"); break;
            case Kind::PowerOff: lay(out, cap, "Power off", nullptr); break;
            case Kind::Signal: {
                char row[COLS + 1];
                _car->row(_car->ctx, it.ref, row, sizeof(row));
                lay(out, cap, row, nullptr);
                break;
            }
            case Kind::Setting: {
                const SEntry& e = Registry::at(it.ref);
                char v[COLS + 1];
                if (_editing && i == _cursor) {
                    char raw[COLS - 1];
                    Registry::formatValue(e, _editValue, raw, sizeof(raw));
                    snprintf(v, sizeof(v), "[%s]", raw);
                } else {
                    Registry::format(*_m, e, v, sizeof(v));
                }
                lay(out, cap, e.tag, v);
                break;
            }
        }
    }

    void Engine::title(char* out, size_t cap) const
    {
        const Level& lv = _stack[_depth];
        const char* t = "MENU";
        if (lv.kind == Kind::Group && lv.ref != ROOT) t = Registry::at(lv.ref).tag;
        else if (lv.kind == Kind::CarRoot)            t = "Car data";
        else if (lv.kind == Kind::CarGroup)           t = _car->groupTag(_car->ctx, lv.ref);
        char pos[8];
        snprintf(pos, sizeof(pos), "%u/%u", (unsigned)(_n ? _cursor + 1 : 0), (unsigned)_n);
        lay(out, cap, t, pos, COLS);
    }

    void Engine::screen(Screen& s) const
    {
        title(s.header, sizeof(s.header));
        // The cursor rides the top row; the last row pair stays full.
        uint8_t top = _cursor;
        if (_n >= 2 && top > _n - 2) top = _n - 2;
        if (_n < 2) top = 0;
        for (uint8_t k = 0; k < 2; ++k) {
            const uint8_t i = top + k;
            if (i >= _n) { s.row[k][0] = 0; continue; }
            s.row[k][0] = i == _cursor ? '>' : ' ';
            item(i, s.row[k] + 1, sizeof(s.row[k]) - 1);
        }
        s.more_up = top > 0;
        s.more_down = top + 2 < _n;
    }
}
