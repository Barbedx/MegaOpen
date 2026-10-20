#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Every task we run, by name, with the three numbers that tell a healthy task
// from a starved or hung one (docs/design/event-tasks.md, Step 1):
//
//   lastAliveMs  when its loop last came round. A critical task more than 2 s
//                stale fails the OTA verdict.
//   wakes        how many times its loop has run since boot.
//   busyMaxUs    the longest single pass this heartbeat window.
//
// Single writer per entry: only the task itself calls beat() on its own id.
// The heartbeat reads every entry without a lock; a torn read of one counter
// costs one wrong number in one log line. The one thing the reader writes is
// a window request, which the owner acts on at its next beat, so busyMaxUs
// still has exactly one writer.
namespace App
{
    struct TaskEntry {
        const char*       name = nullptr;
        TaskHandle_t      handle = nullptr;
        volatile uint32_t lastAliveMs = 0;
        volatile uint32_t wakes = 0;
        volatile uint32_t busyMaxUs = 0;
        bool              critical = false;
        // Window handshake: the reader bumps winReq, the owner copies it into
        // winSeen and restarts busyMaxUs.
        volatile uint32_t winReq = 0;
        uint32_t          winSeen = 0;
    };

    namespace Tasks
    {
        constexpr int MAX = 16;

        // Any task, any time; -1 when the table is full. Stamped alive now, so
        // a task that has not run its first pass yet is not reported late.
        int  add(const char* name, bool critical);
        // The handle, for the stack high-water mark. Set once, after creation.
        void setHandle(int id, TaskHandle_t h);
        // The owner, once per pass of its loop. busyUs = how long the pass took.
        void beat(int id, uint32_t busyUs);

        int              count();
        const TaskEntry* at(int i);

        // Every critical task beat within maxAgeMs. On false, *late names the
        // first one that did not.
        bool criticalAlive(uint32_t nowMs, uint32_t maxAgeMs, const char** late = nullptr);

        // The heartbeat, after reading busyMaxUs: start a new window.
        void newWindow();
    }
}
