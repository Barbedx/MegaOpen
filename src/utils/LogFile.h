#pragma once
#include "../Features.h"
#if FEATURE_LOGFS
#include <Arduino.h>

// The log on flash, so a run in the car can be read afterwards — including what
// happened just before a reboot.
//
// /log/0.log is this boot; every boot shifts the files up one (0 -> 1 ... the
// oldest is deleted), and a file that outgrows FILE_MAX shifts mid-run the same
// way. So "the boot before the reboot" is always /log/1.log.
//
// WHEN IT TOUCHES FLASH (docs/design/event-tasks.md Step 6d, latency L2). Every
// flash erase stops the cache on BOTH cores for the length of a sector (tens of
// ms): keys, the hu ack and the UART/TWAI ISRs all wait. So routine lines stay
// in a RAM buffer, and the file is written only:
//   * when the buffer holds FLUSH_BYTES (16 kB),
//   * on an event: a line at warning level or above ("E [", "W [" from Log::),
//   * on an explicit dump: requestFlush() (the web reading /log/0.log), or
//     flushNow() before a reboot (main.cpp's shutdown handler).
// There is deliberately NO timer: a periodic flush is a periodic stall.
// The cost: a panic or a power cut loses what is still in the buffer (up to
// 16 kB). A software reboot does not (the shutdown handler).
//
// Written only from the LogBus task: sink(), and flushNow() as its onFlush hook.
namespace LogFile
{
    static constexpr int      FILES       = 8;
    static constexpr size_t   FILE_MAX    = 256 * 1024;   // 8 x 256 kB = 2 MB
    static constexpr size_t   FLUSH_BYTES = 16 * 1024;
    // What the log may hold on flash at most (its files plus one buffer that
    // can push the newest past FILE_MAX). The capture recorder budgets the
    // rest of littlefs and never this (src/capture/CanRecorder.h).
    static constexpr size_t   RESERVED_BYTES = FILES * FILE_MAX + 2 * FLUSH_BYTES;

    // Mounts the littlefs partition and rotates. Formats it only when it has
    // never been mounted on this board (NVS "sys/logfsInit"), or when the
    // partition's size is not the one it was last mounted at (NVS
    // "sys/logfsSize": a USB flash of a new partition table moved or resized
    // it, so the old superblock cannot mount there). Any other mount failure
    // is reported and leaves the partition alone.
    bool begin();
    bool mounted();
    // littlefs is mounted, whether or not the log file opened. The capture
    // recorder shares this mount (one LittleFS, mounted here only).
    bool fsMounted();
    // Why begin() failed, for the boot line; "" when mounted.
    const char* mountError();

    // A LogBus::Sink.
    void sink(const char* line, int len, uint32_t ms);

    // Any task: the LogBus task writes the buffer out at its next line.
    void requestFlush();
    // Buffer to file and sync, now. The LogBus task only (LogBus::onFlush,
    // which LogBus::flushAndWait runs there before a reboot).
    void flushNow();

    // For the web API: path of file n, or nullptr if absent; its size.
    const char* path(int n);
    size_t      size(int n);
    size_t      buffered();      // bytes still in RAM, not yet in /log/0.log
    // How many times the files have shifted up since boot: file n read at
    // count c is file n + (rotations() - c) now (LogPush reads past boots).
    uint32_t    rotations();
    size_t      totalBytes();
    size_t      usedBytes();
}
#endif
