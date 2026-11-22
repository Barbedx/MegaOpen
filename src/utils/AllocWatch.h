#pragma once
#include <stdint.h>

// Failed heap allocations, counted (docs/design/stabilisation-1-12.md S3):
// heap_caps_register_failed_alloc_callback, whose callback runs inside the
// failing malloc on any task (and from an ISR, if one allocates). It only
// counts and keeps the last size, caps and task name under a spinlock: no
// log, no allocation, no blocking there (rule R1). A plain malloc under 4096
// bytes that finds no internal RAM falls back to PSRAM first and is no
// failure; what lands here found nothing at all, or asked for internal RAM
// explicitly (WiFi, lwIP's DMA side, BT, a heap_caps_malloc of ours).
//
// Read back by get() (/api/status "allocFail", the [win] line of S6 may print
// it), and said in the log by report() from net_svc: one line when the count
// moved, at most every REPORT_MS:
//   I [mem] alloc failed n=<total> (+<new>) last size=<B> caps=0x<hex> task=<name> at=<ms>
namespace AllocWatch
{
    constexpr uint32_t REPORT_MS = 10000;

    struct Snap {
        uint32_t count = 0;     // failures since boot
        uint32_t size = 0;      // the last one: bytes asked
        uint32_t caps = 0;      // its MALLOC_CAP_* mask
        uint32_t atMs = 0;      // when (millis)
        char     task[16] = ""; // the task it ran on ("isr" in one)
    };

    // setup(), first thing: registers the callback.
    void begin();
    void get(Snap& out);
    // net_svc, every pass: the line above when there is something new.
    void report(uint32_t now);
}
