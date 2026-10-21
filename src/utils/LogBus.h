#pragma once
#include <Arduino.h>
#include "LogRing.h"

// One queue for every log line, and the only place that waits on an output.
//
// WHY: logging used to write Serial synchronously on whatever task logged —
// including the loop task that owns the panel and the keys. 115200 baud is
// ~11 bytes/ms, so the ten heartbeat lines held the loop ~100 ms every 10 s,
// and a burst of [disp] lines held it longer.
//
// line() copies into a RAM ring under a short critical section and returns; a
// full ring drops the line and counts it (dropped()). A low-priority task drains
// the ring into the sinks (Serial, the web log, the flash log), and only that
// task ever blocks on one. Order is preserved; every line carries its uptime.
//
// Any task may call line(); not an ISR. Lines are cut at MAX_LINE.
namespace LogBus
{
    static constexpr int MAX_LINE = 320;   // a [hu] frame with its text column

    // ms = uptime when line() was called, not when the sink runs.
    using Sink = void (*)(const char* line, int len, uint32_t ms);

    // First thing in setup(): on a board with PSRAM, the ring moves out of the
    // small internal boot ring into LOGBUS_RING bytes of PSRAM (or, with no
    // PSRAM found, LOGBUS_RING_INTERNAL bytes of heap), keeping every line.
    // false: nothing could be allocated; the boot ring stays. A no-op without
    // BOARD_HAS_PSRAM, where the ring is static.
    bool grow();
    bool ringInPsram();
    void addSink(Sink s);          // before startTask(); up to 4

    // A line for Serial only, never the web log: the [hu] echo of
    // a frame FROM the head unit ("[hu] R ..."), which the [hur] line already
    // carries decoded. In the ring like every line; its readers skip it.
    inline bool serialOnly(const char* line) { return !strncmp(line, "[hu] R ", 7); }
    void line(const char* s);      // never blocks
    void printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    void startTask(UBaseType_t prio, BaseType_t core);

    // The task, for its stack high-water mark; null before startTask().
    TaskHandle_t task();
    // Called on the LogBus task after every wake (a drain, or the 100 ms
    // timeout), with how long the drain took. How main.cpp puts this task in
    // the task registry without utils/ depending on app/.
    using WakeHook = void (*)(uint32_t busyUs);
    void onWake(WakeHook h);

    // For a reboot (main.cpp's shutdown handler): wakes the LogBus task to
    // drain the ring through every sink and then run the onFlush() hook (the
    // flash log's sync), and waits for it. Only the LogBus task ever runs a
    // sink. Gives up after timeoutMs (false): a reboot must not hang on its log.
    bool flushAndWait(uint32_t timeoutMs);
    void onFlush(void (*fn)());

    // Lines lost to a full ring since boot. The sinks also get a
    // "[log] N line(s) dropped" line before the next record after a loss.
    uint32_t dropped();
    uint32_t lines();              // lines accepted since boot = the newest line's number
    int      ringSize();

    // The web log, GET /api/logs?since=N. Every accepted line has a number
    // (1, 2, ...); the ring keeps the lines already sent to the sinks until it
    // needs their room (LogRing.h), so a reader polls for what came after N.
    using Cursor = LogRing::Cursor;
    Cursor after(uint32_t since);
    // The next line into buf (MAX_LINE + 1), its uptime in ms; its length, or
    // -1 when the reader is up to date (c.n - 1 is then the newest line). Lines
    // the ring evicted before the reader got to them are added to lost. Any
    // task; holds the ring's lock only for short steps, never for the caller.
    int read(Cursor& c, char* buf, uint32_t& ms, uint32_t& lost);
}
