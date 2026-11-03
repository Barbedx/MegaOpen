#pragma once
// A trap for the heap corruption seen once on the bench (2026-11-02, boot 190:
// `multi_heap_free: head != NULL` in esp_http_client_cleanup on the log push's
// task, the http_parser block right after the client struct, writer unknown).
// A diagnostic build only, nothing of it in a release image:
//   PLATFORMIO_BUILD_FLAGS=-DHEAP_SENTINEL=1 pio run -e t2can
//
// WHAT IT DOES. check(where) walks every internal heap
// (heap_caps_check_integrity: every block's head and tail canary, ~1 ms,
// under the heap lock). The first failing walk writes to the log what the
// prebuilt checker only says on the console — "CORRUPT HEAP: Bad head at
// 0x3fcdb820. Expected 0xabba1234 got 0x..." — plus 128 bytes around that
// address in hex (what was written says who wrote it), the task and the step
// it was found at; the flash log is written, then the board aborts so the
// coredump keeps every task's stack at that moment. The next boot pushes
// both to the log repository.
//
// HOW IT READS THE CHECKER. The checker prints with esp_rom_printf, which
// goes through the ROM console's putc (channel 1 = UART0 here, which nobody
// reads on the T-2CAN). begin() installs its own putc there that keeps the
// last 512 characters in a DRAM ring and forwards each to the UART as before.
//
// WHERE IT IS CALLED. The log push's job at every step (src/net/LogPush.cpp),
// the GitHub job's steps and its queue wait (src/net/GhOta.cpp), every web
// request before and after its route (src/net/WebUi.cpp), and net_svc every
// 500 ms. The [heap] line once a minute says how many walks and the longest.
#if HEAP_SENTINEL
#include <stdint.h>
namespace HeapSentinel
{
    // First thing in setup(): the ROM putc hook.
    void begin();
    // true: every internal heap is intact. false: the report is in the log
    // and the board is aborting (the first finder; later callers just get false).
    bool check(const char* where);
    uint32_t checks();   // walks so far
    uint32_t maxUs();    // the longest walk
}
#define HEAP_CHECK(where) HeapSentinel::check(where)
#else
#define HEAP_CHECK(where) ((void)0)
#endif
