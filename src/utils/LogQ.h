#pragma once
#include <stdint.h>

// The log of a foreign context (docs/design/stabilisation-1-12.md, R1): the
// WiFi/USB event tasks, esp_event handlers, tud_*_cb, ISRs. Their stacks are
// not ours (the core's USB event task has 2 kB, and a LOGI there overflowed it,
// car boots 289/290), so they only count, flag, post or notify.
//
// LOGQ(cat, fmt, a0..a3) is that post: a 28-byte record (the literal tag and
// format, up to four numbers, the uptime) into a 64-slot ring under a portMUX,
// no formatting, no stack beyond a few words. The logbus task takes the records
// and formats them into ordinary INF lines with their own uptime (Log.cpp,
// drainQueued). A full ring drops the record and counts it: the [hb] line says
// `logq drops=N`.
//
// Numbers only (integers, enums, bools; each cast to uint32_t): no %s, no
// pointer the record would outlive. The format sees them as 32-bit words, so
// %u %d %x %lu %c work, %llu and %f do not. Any task and any ISR.
namespace LogQ
{
    struct Rec {
        const char* cat;     // a string literal
        const char* fmt;     // a string literal
        uint32_t    a[4];
        uint32_t    ms;      // uptime at post()
    };
#if UINTPTR_MAX == 0xFFFFFFFFu
    static_assert(sizeof(Rec) == 28, "LOGQ: the record is 28 bytes");
#endif
    static constexpr int SLOTS = 64;

    // Never blocks, never formats, never logs. In IRAM: safe in an ISR that
    // runs with the cache off.
    void post(const char* cat, const char* fmt, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);

    // The logbus task: who post() notifies (null: the drain's 100 ms timeout).
    void setWake(void* task);
    // The logbus task: the oldest record into r; false when none waits.
    bool take(Rec& r);

    uint32_t posted();    // records accepted since boot
    uint32_t dropped();   // records lost to a full ring since boot
}
