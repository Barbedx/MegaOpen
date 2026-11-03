#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../Features.h"

// The one network worker: the GitHub update (GhOta) and the log push
// (LogPush) run their jobs here, one after the other, on a task that lives
// for the whole boot with a static stack.
//
// WHY ONE TASK. Each job used to create a task of its own (10 kB and 8 kB of
// internal stack, every minute for the push) and delete it after; the stack
// came out of the internal heap and went back through IDLE0, so the heap
// around the HTTP client's small blocks was churned by every job, and a
// mutex (TlsJob) plus a 100 ms poll serialised the two anyway. One permanent
// task is the same serialisation without the churn and the poll. Both jobs
// move mbedTLS's allocator into PSRAM while they run (InPsram, in the .cpp):
// that is a global, which is the other reason they never overlap.
//
// IDLE's priority on the WiFi core (TaskPlan.h NETJOB_*): a run of TLS
// handshakes at 1 kept IDLE0 out for 5 s and the task watchdog rebooted the
// board (the car, 12 boots, 1.6.0-rc5).
#if FEATURE_GH_OTA || FEATURE_LOG_PUSH
namespace NetJob
{
    using Fn = void (*)(void* arg);

    // setup(), once, before anything posts. The task and its queue are static.
    void begin();
    // Queue a job; it runs after the ones before it, with mbedTLS in PSRAM.
    // `after`, if given, runs right after it with mbedTLS back on its own
    // allocator (GhOta's reboot into the trial). false: the queue (4) is full,
    // nothing was queued. `name` is a literal, kept by pointer.
    bool post(const char* name, Fn fn, void* arg, Fn after = nullptr);
    // The job running now (its name), nullptr when none; how many wait.
    const char* running();
    uint8_t     waiting();
    // A job is running or waiting.
    bool busy();
    // On the worker: the running job's mbedTLS allocates in PSRAM.
    bool tlsInPsram();
}
#endif
