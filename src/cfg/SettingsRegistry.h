#pragma once
#include <stddef.h>
#include <stdint.h>
#include "Settings.h"
#include "../knobs/KnobConfig.h"
#include "../vehicle/AlertRules.h"

// Every setting the user can change, described once: the web form and the
// panel menu are both drawn from this table, so they cannot disagree on a
// range, a name or which values exist. It wraps the storage that already
// exists (Cfg::Settings, the knobs blob); it stores nothing of its own and
// changes no NVS key.
//
// Pure (no Arduino): the native tests walk the very table the board uses.
//
// Values travel as int32_t. A Bool is 0/1; an Enum is its row index, whatever
// the field stores (a String id or an enum class), so the menu can step it.
// Strings the panel cannot edit (SSID, passwords, mDNS name) are not here.
//
// A Table is a fixed grid of such cells (the panel alerts: 16 rules x 5),
// read and written a cell at a time; the table's own put() is the check, so
// a cell whose range depends on another (a threshold on its row's signal) is
// refused by the code that knows why. Tables are web-only.
namespace Cfg
{
    // What a registry entry reads and writes. `k` is null on a board without
    // knobs, `a` where the caller has no alert rules at hand; their entries
    // then report unavailable.
    struct Model {
        Settings*            s = nullptr;
        Knobs::Config*       k = nullptr;
        Vehicle::AlertRules* a = nullptr;
    };

    enum class SType : uint8_t { Group, Bool, Int, Enum, Table };

    // Where an entry is offered.
    enum : uint8_t {
        F_WEB  = 0x01,     // the settings form
        F_MENU = 0x02,     // the panel menu
        F_RO   = 0x04,     // shown, never written from outside (net_svc owns it)
    };

    // Who must hear about a change. The registry only edits the model; the
    // caller (WebUi, the menu glue) stages the change for the task that owns
    // it, one hand-off per bit, then saves.
    enum : uint8_t {
        AP_NONE   = 0x00,
        AP_UI     = 0x01,  // ui task: media screen, pane, popups
        AP_KEYS   = 0x02,  // keys task: timing, pulse, links, routing
        AP_KNOBS  = 0x04,  // knob service: lights, encoders (knobs blob)
        AP_MEDIA  = 0x08,  // media router / HU text learner
        AP_CAR    = 0x10,  // car task: the alert rules (own NVS key)
        AP_REBOOT = 0x80,  // read once at boot: saved, applied by a reboot
    };

    // An enum's rows: id is what is stored and sent (ASCII, so the panel can
    // show it), name is the page's Ukrainian label.
    struct SEnum {
        uint8_t     (*count)();
        const char* (*id)(uint8_t i);
        const char* (*name)(uint8_t i);
    };

    // A Table's column: Bool, Int or Enum. An Enum cell is its row index in
    // `en`; an Int's min/max are what the page offers, the table's put() has
    // the last word.
    struct SColumn {
        const char*  id;       // ASCII, the page's key
        const char*  label;    // Ukrainian, the web
        SType        type;
        int32_t      min, max;
        const SEnum* en;
    };

    struct STable {
        uint8_t        rows, cols;
        const SColumn* col;
        int32_t (*get)(const Model&, uint8_t row, uint8_t col);
        // False: refused, the cell unchanged. Cells of a row are written in
        // column order, so a cell may depend on the ones before it.
        bool    (*put)(Model&, uint8_t row, uint8_t col, int32_t v);
        // An Int cell's decimal places (the value is a scaled integer); may
        // depend on its row. Null = whole numbers.
        uint8_t (*decimals)(const Model&, uint8_t row, uint8_t col);
    };

    struct SEntry {
        const char*  id;       // stable API key (the /api/settings JSON key)
        const char*  label;    // Ukrainian, the web
        const char*  tag;      // ASCII, <= TAG_MAX, the panel
        const char*  parent;   // a Group's id; nullptr = top level
        SType        type;
        uint8_t      flags;
        uint8_t      apply;
        int32_t      min, max; // Int: inclusive range. Enum/Bool: derived
        uint16_t     step;     // Int: menu step (0 = 1)
        const char*  unit;     // Int: shown after the value, may be nullptr
        const SEnum* en;       // Enum only
        int32_t (*get)(const Model&);
        void    (*put)(Model&, int32_t);   // gets a value valid() accepted
        const STable* tab;     // Table only (get/put are then null)
    };

    namespace Registry
    {
        // The panel shows "tag  [value]" on a 26-column row.
        inline constexpr uint8_t TAG_MAX = 14;

        size_t        count();
        const SEntry& at(size_t i);
        const SEntry* find(const char* id);
        int           indexOf(const char* id);     // -1: no such entry

        // False when the entry's storage is missing from this model (knobs).
        bool    available(const Model& m, const SEntry& e);

        // Enum and Bool ranges come from their rows.
        int32_t minOf(const SEntry& e);
        int32_t maxOf(const SEntry& e);
        bool    valid(const SEntry& e, int32_t v);
        // Into range; Int also onto its step grid from min.
        int32_t clamp(const SEntry& e, int32_t v);

        int32_t get(const Model& m, const SEntry& e);
        // Refuses (false, nothing written) a group, an F_RO entry, an
        // unavailable one or an invalid value. On success ORs the entry's
        // apply bits into *apply, when given; an unchanged value adds nothing.
        bool    set(Model& m, const SEntry& e, int32_t v, uint8_t* apply = nullptr);

        // One step of the menu's edit: Int by step, Enum to the next row,
        // Bool flips; wraps for Enum/Bool, stops at the ends for Int.
        int32_t stepped(const SEntry& e, int32_t v, int8_t dir);

        // Enum: row index for an id, -1 if unknown. And back.
        int         enumIndex(const SEntry& e, const char* id);
        const char* enumId(const SEntry& e, int32_t v);
        const char* enumName(const SEntry& e, int32_t v);

        // The value as the panel shows it, ASCII, NUL-terminated, cut to cap:
        // the stored one ("--" when unavailable), or any value of the entry.
        void    format(const Model& m, const SEntry& e, char* out, size_t cap);
        void    formatValue(const SEntry& e, int32_t v, char* out, size_t cap);

        // ---- Table cells (get()/set() of a Table read 0 and refuse) ----------
        // A bad row or column reads 0. setCell refuses (false, nothing
        // written) what set() would, a bad cell, an Enum/Bool value outside
        // its rows, or whatever the table's put() refuses; an unchanged
        // value adds no apply bits.
        int32_t cell(const Model& m, const SEntry& e, uint8_t row, uint8_t col);
        bool    setCell(Model& m, const SEntry& e, uint8_t row, uint8_t col, int32_t v,
                        uint8_t* apply = nullptr);
        uint8_t cellDecimals(const Model& m, const SEntry& e, uint8_t row, uint8_t col);
        // A column's offered range (Enum/Bool from their rows).
        int32_t colMin(const SColumn& c);
        int32_t colMax(const SColumn& c);
        int     colEnumIndex(const SColumn& c, const char* id);   // -1: unknown

        // A scaled integer with its decimal point placed (1368, 2 -> "13.68"),
        // and a page's number back to one (false: not finite, or too big).
        const char* decText(char* out, size_t cap, int32_t v, uint8_t decimals);
        bool        decParse(double x, uint8_t decimals, int32_t& out);
    }
}
