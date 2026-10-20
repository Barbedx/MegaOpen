#include "CanSniffer.h"
#include <string.h>

namespace Vehicle
{
    const CanSniffer::Entry* CanSniffer::find(uint32_t id) const
    {
        for (uint8_t i = 0; i < _count; ++i)
            if (_e[i].used && _e[i].id == id) return &_e[i];
        return nullptr;
    }

    void CanSniffer::applyRequests()
    {
        if (_wantReset)    { _wantReset = false; _wantBaseline = false; reset(); }
        if (_wantBaseline) { _wantBaseline = false; markBaseline(); }
    }

    bool CanSniffer::copyAt(uint8_t i, Entry& out) const
    {
        if (i >= MAX_IDS) return false;
        for (uint8_t tries = 0; tries < 8; ++tries) {
            const uint32_t s1 = _seq;
            if (s1 & 1u) continue;
            __sync_synchronize();
            memcpy(&out, (const void*)&_e[i], sizeof(Entry));
            __sync_synchronize();
            if (_seq == s1) return true;
        }
        return false;
    }

    void CanSniffer::ingest(uint32_t id, const uint8_t* data, uint8_t len, uint32_t nowMs)
    {
        applyRequests();
        if (!_enabled) return;
        ++_frames;
        if (len > 8) len = 8;

        for (uint8_t i = 0; i < _count; ++i) {
            Entry& e = _e[i];
            if (!e.used || e.id != id) continue;

            writeBegin();
            for (uint8_t b = 0; b < len; ++b) {
                if (data[b] != e.last[b]) {
                    e.changed |= (uint8_t)(1u << b);
                    e.last[b] = data[b];
                }
            }
            e.len   = len;
            e.count++;
            e.lastMs = nowMs;
            writeEnd();
            return;
        }

        if (_count >= MAX_IDS) { ++_overflow; return; }

        writeBegin();
        Entry& e = _e[_count];
        e.used    = true;
        e.id      = id;
        e.count   = 1;
        e.firstMs = e.lastMs = nowMs;
        e.len     = len;
        e.changed = 0;               // first sighting is the baseline, not a change
        for (uint8_t b = 0; b < 8; ++b) e.last[b] = b < len ? data[b] : 0;
        _count = (uint8_t)(_count + 1);   // after the row is whole
        writeEnd();
    }

    void CanSniffer::markBaseline()
    {
        writeBegin();
        for (uint8_t i = 0; i < _count; ++i) _e[i].changed = 0;
        writeEnd();
    }

    void CanSniffer::reset()
    {
        writeBegin();
        _count = 0;
        for (uint8_t i = 0; i < MAX_IDS; ++i) _e[i] = Entry{};
        _frames = 0;
        _overflow = 0;
        writeEnd();
    }
}
