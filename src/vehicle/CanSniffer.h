#pragma once
#include <stdint.h>

// Per-ID CAN observation table for finding which frame carries which signal.
// One row per ID: last payload plus a mask of bytes that changed since
// markBaseline(). Workflow: mark baseline, do something (open a door), look for
// IDs with a non-zero mask. Portable (time arrives as nowMs), native-tested.
namespace Vehicle
{
    class CanSniffer {
    public:
        static constexpr uint8_t MAX_IDS = 64;

        struct Entry {
            uint32_t id;
            uint32_t count;
            uint32_t firstMs;
            uint32_t lastMs;
            uint8_t  len;
            uint8_t  last[8];
            uint8_t  changed;    // bit per byte: this byte differed at least once
            bool     used;
        };

        // ONE WRITER. ingest(), markBaseline(), reset() and applyRequests()
        // run on the task that feeds the table: the car task when the board has
        // a car source, else the panel task (main.cpp, snifferOwner).
        // Another task (httpd) asks with requestBaseline()/requestReset(),
        // which the owner applies at its next ingest() or applyRequests(), and
        // reads rows with copyAt(), which never returns a half-written row.
        void ingest(uint32_t id, const uint8_t* data, uint8_t len, uint32_t nowMs);

        // Forget the value history but keep the rows: the "start watching" action.
        void markBaseline();
        // Forget everything, including which IDs exist.
        void reset();

        // From any task: never touches the table.
        void requestBaseline() { _wantBaseline = true; }
        void requestReset()    { _wantReset = true; }
        // The owner: applies what was asked. ingest() calls it first; the
        // owner also calls it when no frames arrive, so a reset still lands.
        void applyRequests();

        // Off: ingest() returns at once and counts nothing, so a busy car bus
        // costs no table search per frame (docs/design/event-tasks.md Step 5:
        // off by default on the T-2CAN, where the capture recorder is the
        // tool). Any task may flip it; the owner reads it per frame. Turning it
        // on keeps whatever the table held.
        void setEnabled(bool on) { _enabled = on; }
        bool enabled() const { return _enabled; }

        uint8_t count() const { return _count; }
        // Owner only (or single-task code such as the native tests).
        const Entry& at(uint8_t i) const { return _e[i < MAX_IDS ? i : 0]; }
        // Any task: a consistent copy of row i; false if it kept changing
        // under the copy or is out of range.
        bool copyAt(uint8_t i, Entry& out) const;
        const Entry* find(uint32_t id) const;

        uint32_t frames() const { return _frames; }
        // IDs seen after the table filled up; the UI must show it when non-zero.
        uint32_t overflow() const { return _overflow; }

    private:
        // A sequence lock: odd while the owner is writing.
        void writeBegin() { ++_seq; __sync_synchronize(); }
        void writeEnd()   { __sync_synchronize(); ++_seq; }

        Entry   _e[MAX_IDS] = {};
        volatile uint8_t  _count = 0;
        volatile uint32_t _seq = 0;
        volatile bool     _wantBaseline = false;
        volatile bool     _wantReset = false;
        volatile bool     _enabled = true;
        uint32_t _frames = 0;
        uint32_t _overflow = 0;
    };
}
