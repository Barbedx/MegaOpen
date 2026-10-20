#pragma once
#include <stdint.h>

// The two locks Snapshot<T> and Staged<T> are built on, in one place so the
// host build can stand them in (docs/design/event-tasks.md §3.1).
//
//   Spin   a portMUX critical section: interrupts off on this core, the other
//          core spins. Only for a copy of a few dozen bytes — never a call, a
//          log line or an allocation inside it.
//   Mutex  a FreeRTOS mutex, statically allocated so construction can neither
//          fail nor allocate (it may run before the scheduler starts). An
//          owner on the hot path only ever try-takes it; httpd may wait.
//
// On the host both are an atomic flag: the tests are single-threaded, and what
// they need is the try-take answering "busy" while somebody holds it, which a
// std::mutex does not promise to the thread that already holds it.
#ifdef NATIVE
#include <atomic>
namespace Locks
{
    class Spin {
    public:
        void lock()   { while (_f.test_and_set(std::memory_order_acquire)) {} }
        void unlock() { _f.clear(std::memory_order_release); }
    private:
        std::atomic_flag _f = ATOMIC_FLAG_INIT;
    };

    class Mutex {
    public:
        bool take(uint32_t /*waitMs*/) { return tryTake(); }
        bool tryTake() { return !_f.test_and_set(std::memory_order_acquire); }
        void give()    { _f.clear(std::memory_order_release); }
    private:
        std::atomic_flag _f = ATOMIC_FLAG_INIT;
    };
}
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
namespace Locks
{
    class Spin {
    public:
        void lock()   { portENTER_CRITICAL(&_mux); }
        void unlock() { portEXIT_CRITICAL(&_mux); }
    private:
        portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    };

    class Mutex {
    public:
        Mutex() { _h = xSemaphoreCreateMutexStatic(&_buf); }
        Mutex(const Mutex&) = delete;
        Mutex& operator=(const Mutex&) = delete;
        bool take(uint32_t waitMs) { return xSemaphoreTake(_h, pdMS_TO_TICKS(waitMs)) == pdTRUE; }
        bool tryTake()             { return xSemaphoreTake(_h, 0) == pdTRUE; }
        void give()                { xSemaphoreGive(_h); }
    private:
        StaticSemaphore_t _buf;
        SemaphoreHandle_t _h = nullptr;
    };
}
#endif
