#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../cfg/SettingsRegistry.h"

// The panel menu: a tree drawn from the settings registry, the car's live
// signals and two actions, walked with four commands. It decides what the
// two rows say; it never renders, never stores, never sees a key event.
//
//   MENU
//   ├─ < Exit     closes the menu (the cursor opens on the row below it)
//   ├─ the registry's top-level F_MENU groups (Display, Lights), each a
//   │  list of its available children; a child group opens another level
//   ├─ Car data   one row per signal group the car has data for, then the
//   │             signals of that group, read-only
//   └─ Power off
//
// Every level below the root starts with "< Back". Next/Prev move the cursor
// (wrapping); Select opens, toggles a Bool, or starts editing an Int/Enum,
// whose value is then shown [in brackets] and stepped by Next/Prev until a
// second Select commits it. Close shuts the menu from any depth.
//
// A commit is reported, not applied: the caller hands the entry and value to
// whoever owns the settings, and the next render reads the stored value.
//
// Pure (no Arduino): the native tests drive it with a fake car.
namespace MenuUi
{
    inline constexpr uint8_t COLS = 26;        // a row of the panel's list screen

    // What the menu knows of the car, from the glue. Signals are ids the
    // glue understands; the menu only lists and prints them.
    struct CarView {
        uint8_t     (*groups)(void* ctx) = nullptr;
        const char* (*groupTag)(void* ctx, uint8_t g) = nullptr;     // ASCII
        // The signals of group g that have a value now, at most cap.
        uint8_t     (*signals)(void* ctx, uint8_t g, uint8_t* out, uint8_t cap) = nullptr;
        // One row, ASCII, already laid out ("eng_temp      92C").
        void        (*row)(void* ctx, uint8_t sig, char* out, size_t cap) = nullptr;
        void*       ctx = nullptr;
    };

    enum class Cmd : uint8_t { Next, Prev, Select, Close };

    enum class Out : uint8_t {
        None,       // nothing changed
        Redraw,     // the rows changed
        Commit,     // a value to store: commitEntry() / commitValue(); redraw too
        Closed,     // the menu shut
        PowerOff,   // "Power off" chosen; the menu shut
    };

    class Engine {
    public:
        // `m` is read for values and availability, and a commit is written
        // into it at once: it is the menu's own copy, so the row shows the new
        // value, and a second press reads it, before the owner has stored the
        // first (a Bool pressed twice toggles twice). The owner re-reads its
        // copy once its commits are through.
        void begin(Cfg::Model* m, const CarView* car = nullptr);

        // Opens at the root, cursor on the first row after "< Exit".
        void open();
        void close();
        bool isOpen() const { return _open; }
        bool editing() const { return _editing; }

        Out key(Cmd c);

        // Valid after Out::Commit.
        const Cfg::SEntry* commitEntry() const { return _commitEntry; }
        int32_t            commitValue() const { return _commitValue; }

        // ---- the screen -----------------------------------------------------
        // The header, a list of up to two visible rows (the cursor's row marked
        // '>'), and whether more rows lie above / below the window.
        struct Screen {
            char header[COLS + 1];
            char row[2][COLS + 1];
            bool more_up, more_down;
        };
        void screen(Screen& s) const;

        // The current level, for the tests and the log.
        uint8_t count() const { return _n; }
        uint8_t cursor() const { return _cursor; }
        uint8_t depth() const { return _depth; }
        void    item(uint8_t i, char* out, size_t cap) const;   // unmarked row text

        // Rebuild the current level (signals come and go); keeps the cursor
        // on the same row when it is still there.
        void refresh();

        static constexpr uint8_t MAX_ITEMS = 64;
        static constexpr uint8_t MAX_DEPTH = 4;

    private:
        enum class Kind : uint8_t { Back, Exit, Group, Setting, CarRoot, CarGroup, Signal, PowerOff };
        struct Item { Kind kind; uint8_t ref; };   // ref: registry index, car group or signal
        struct Level { Kind kind; uint8_t ref; uint8_t cursor; };

        void build();
        bool groupHasItems(uint8_t regIndex) const;
        void enter(Kind kind, uint8_t ref);
        void leave();
        void title(char* out, size_t cap) const;

        Cfg::Model*       _m = nullptr;
        const CarView*    _car = nullptr;
        bool    _open = false;
        bool    _editing = false;
        int32_t _editValue = 0;
        Level   _stack[MAX_DEPTH];
        uint8_t _depth = 0;               // 0 = root
        Item    _items[MAX_ITEMS];
        uint8_t _n = 0;
        uint8_t _cursor = 0;
        const Cfg::SEntry* _commitEntry = nullptr;
        int32_t _commitValue = 0;
    };
}
