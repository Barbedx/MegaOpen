#pragma once
#include <Arduino.h>

// The supervisor (docs/design/stabilisation-1-12.md, S6): every task in the
// registry with a limit in Plan::LATE_LIMITS (TaskPlan.h) that has not beaten
// for longer than it is said once, and said again when it beats:
//
//   W [sup] late task=<name> age=<ms> limit=<ms>
//   I [sup] task=<name> back after <ms> ms
//
// It only logs: 1.12 reboots nothing (FEATURE_TASK_WDT is the opt-in). It runs
// on the logbus task, not on net_svc, which it watches: main.cpp calls tick()
// from LogBus's wake hook (every drain, and at least every 100 ms).
namespace App
{
    namespace Supervisor
    {
        // The end of setup(), on the loop task, right after the loop's first
        // beat: nothing is watched before (setup() holds the loop for
        // seconds). With FEATURE_TASK_WDT it subscribes the loop to the task
        // watchdog.
        void begin();
        // The logbus task. Looks at most every CHECK_MS.
        void tick(uint32_t nowMs);
        constexpr uint32_t CHECK_MS = 100;
    }
}
