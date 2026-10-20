#pragma once
#include <stddef.h>
#include <stdint.h>
#include "VehicleState.h"
#include "../utils/UiList.h"

// Panel alerts: "show it on the display when this signal...". A fixed table of
// 16 rules in NVS (key "alerts"), evaluated on the car task against State.
// Portable and native-tested (test_decode).
//
// The table is shaped for the settings registry's Table type (agreed with the
// panel-menu work): five int columns, read and written by get()/set(), and a
// blob for NVS. Column order: sig, cond, thr, cooldownS, on.
//
// The only factory rule is the car's own low-fuel warning, on its rising
// edge: the point is to see the cluster's lamp and the popup arrive together.
// No threshold presets (the owner's call): any other rule is the user's.
namespace Vehicle
{
    enum AlertCond : uint8_t { AC_ABOVE = 0, AC_BELOW, AC_RISES, AC_FALLS, AC_CHANGES, AC_COUNT };
    inline constexpr UiName ALERT_COND[] = {
        {"above", "вище"}, {"below", "нижче"}, {"rises", "зʼявилось"},
        {"falls", "зникло"}, {"changes", "змінилось"},
    };
    static_assert(sizeof(ALERT_COND) / sizeof(ALERT_COND[0]) == AC_COUNT, "ALERT_COND is out of step");

    struct AlertRule {
        uint8_t  sig = SIG_COUNT;   // SIG_COUNT = an empty row
        uint8_t  cond = AC_RISES;
        int32_t  thr = 0;           // the signal's scaled integer (Meta.decimals)
        uint16_t cooldownS = 0;     // at least this long between two popups
        bool     on = false;
    };

    class AlertRules {
    public:
        static constexpr uint8_t ROWS = 16, COLS = 5;
        enum Col : uint8_t { C_SIG = 0, C_COND, C_THR, C_COOLDOWN, C_ON };
        static constexpr uint32_t COOLDOWN_MAX_S = 24 * 3600;

        // Blob v1: [1] version, then per row: sig, cond, thr i32, cooldownS u16, on.
        static constexpr uint8_t ROW_BYTES = 9;
        static constexpr size_t  BLOB_BYTES = 1 + ROWS * ROW_BYTES;

        AlertRule r[ROWS];

        // The low-fuel rule alone.
        void factory();

        // The registry's view. get() of a bad cell is 0; set() refuses (false,
        // nothing changed) a bad cell or a value out of the column's range:
        // sig 0..SIG_COUNT (SIG_COUNT empties the row), cond 0..AC_COUNT-1,
        // thr within the signal's range, cooldownS 0..COOLDOWN_MAX_S, on 0/1.
        int32_t get(uint8_t row, uint8_t col) const;
        bool    set(uint8_t row, uint8_t col, int32_t v);

        size_t toBlob(uint8_t* out) const;                 // BLOB_BYTES
        bool   fromBlob(const uint8_t* in, size_t n);      // false: unchanged
    };

    // The rules against State, one tick at a time (the car task, ~10 Hz).
    //
    // above/below: fires when the value crosses the threshold, then stays
    // quiet until it comes back past it by a hysteresis band (1/50 of the
    // signal's range, at least 1) AND the cooldown has run out.
    // rises/falls/changes: fires on the edge itself (a yes/no that is already
    // on when first seen counts as rising: the lamp lit at ignition).
    //
    // ONE popup is pending at a time, newest wins. It waits while the glass is
    // held (a menu, a person's screen), and is dropped if its condition has
    // cleared by then: an alert never covers a menu, and never shows a state
    // that is already over.
    class AlertEval {
    public:
        struct Fire {
            uint8_t row = 0, sig = SIG_COUNT;
            int32_t value = 0;
        };

        void tick(const AlertRules& rules, const State& s, uint32_t nowMs);
        // The pending popup, when the glass is free and it still holds.
        bool take(const AlertRules& rules, const State& s, bool glassFree, Fire& out);

        uint32_t fired() const       { return _fired; }
        uint32_t overwritten() const { return _overwritten; }
        uint32_t dropped() const     { return _dropped; }

        // "fuel_low: 1", "batt_v: 11.80 V", plus " ?" when unverified.
        static int text(char* b, size_t cap, uint8_t sig, int32_t v, bool verified);

    private:
        struct Row {
            AlertRule rule;          // the rule this state belongs to
            bool      seen = false;
            int32_t   last = 0;
            bool      armed = true;
            bool      everFired = false;
            uint32_t  lastFireMs = 0;
        };
        static bool same(const AlertRule& a, const AlertRule& b);
        static bool holds(const AlertRule& r, int32_t v, const Fire& f);

        Row      _row[AlertRules::ROWS];
        Fire     _pend;
        bool     _has = false;
        uint32_t _fired = 0, _overwritten = 0, _dropped = 0;
    };
}
