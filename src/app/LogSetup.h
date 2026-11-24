#pragma once
#include <Arduino.h>
#include "../cfg/Settings.h"

// The log's wiring at boot (docs/design/logging.md): the LogBus sinks (USB
// serial, the flash log, the log push), the flush before a software reboot,
// the IDF's esp_log tap and the saved levels. setup() calls these in this order.
namespace App
{
    namespace LogSetup
    {
        // First thing: the serial sink, and the flash log when it mounts.
        void sinks();
        // The flash log mounted (FEATURE_LOGFS); false without one.
        bool flashOn();
#if FEATURE_LOG_PUSH
        // The push's spool and sink; `snapshot` is the status object it sends
        // every LogPush::SNAP_MS.
        void push(size_t (*snapshot)(char* out, size_t cap));
#endif
        // After LogBus::startTask: the flush before every software reboot.
        void onShutdown();
        // The [log] lines after the boot line: where the ring is, the flash log.
        void report(bool ringGrown);
        // IDF/library lines into the LogBus as "[idf] ...".
        void tapIdf();
        // The saved levels, tags, sinks and status interval.
        void apply(const Cfg::Settings& s);
    }
}
