#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "TaskPlan.h"
#include "TaskRegistry.h"

// Task creation, one way for every task we own: pinned (or deliberately not)
// by the TaskPlan numbers, and entered in the registry so it shows up in the
// [task] heartbeat and counts toward the OTA verdict when it is critical.
namespace App
{
    struct TaskSpec {
        const char*    name = nullptr;
        TaskFunction_t fn = nullptr;
        void*          arg = nullptr;
        uint32_t       stack = 4096;
        UBaseType_t    prio = 1;
        int            core = -1;         // through App::core(): -1 = either core
        bool           critical = false;  // its liveness gates the OTA verdict
        // Subscribe to the task watchdog. Accepted and IGNORED until Step 6:
        // a task with a hidden long block would reboot-loop the box, so the
        // [task] busyMax numbers get read first.
        bool           wdt = false;
        // Where the registry id goes. Written BEFORE the task is created, so
        // the task can read it on its first pass even if it preempts spawn().
        int*           idOut = nullptr;
    };

    // The task handle, or nullptr when creation failed (logged). A critical
    // task that failed to start stays in the registry, never beats, and so
    // fails the OTA trial: deliberate, see Rt.cpp.
    TaskHandle_t spawn(const TaskSpec& s);
}
