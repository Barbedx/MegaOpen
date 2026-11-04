#pragma once
#include <Arduino.h>

// Boot evidence: why this boot happened, the boot and crash counters (NVS
// "sys"), the last crash from the coredump partition, and the heap at each
// setup() stage.
namespace App
{
    namespace Boot
    {
        // Free heap at each setup() stage, with the delta since the last mark.
        void memMark(const char* what);
        // Why this boot happened, and how many there have been: tells a real
        // reboot (panic, watchdog, brownout) from a freeze that only looked
        // like one. Then the last crash, if the coredump partition has one.
        void logReason();
    }
}
