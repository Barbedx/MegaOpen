#pragma once
#include <Arduino.h>

// net_svc: everything that is neither a key nor the panel (docs/design/event-
// tasks.md, Step 3). Priority 1 on core 0, with WiFi, lwIP and httpd, woken
// every 20 ms.
//
// It used to be the second half of loop(): the WiFi state machine, the
// heartbeat (whose WiFi.SSID() and String work allocate) and the OTA
// verdict (App::OtaTrial) all ran on the task that owns the panel, between two poll()s.
// Here a slow WiFi call costs a late heartbeat line, not a late panel frame.
//
// The verdict moved with it, which is what makes it honest: it now runs on a
// different task from the loop, so a hung loop (a critical task in the
// registry) is seen as late and the trial does not confirm. On the loop it
// could only ever see itself alive.
//
// What is generic lives here: the pacing, the heap low-water sampling and its
// [win] line, the [task] line, and the verdict. What each line says about the
// rest of the firmware comes from main.cpp through the hooks.
namespace App
{
    namespace Housekeeping
    {
        struct Hooks {
            // Every pass (20 ms): the web server (ElegantOTA).
            void (*web)(uint32_t nowMs) = nullptr;
            // Every pass: the WiFi state machine.
            void (*wifi)(uint32_t nowMs) = nullptr;
            // WiFi has been usable at least once since boot (STA joined, or our
            // own AP up): the next OTA can arrive.
            bool (*wifiSeen)() = nullptr;
            // Every 10 s, after [win] and [task]: the heartbeat lines that have
            // no other owner.
            void (*beat)(uint32_t nowMs) = nullptr;
            // Once a minute, and on the first pass: the String-heavy [net] line.
            void (*beatSlow)(uint32_t nowMs) = nullptr;
        };

        // Creates net_svc. Critical: it serves the web, which is how the next
        // OTA arrives, and it now gives the verdict.
        void start(const Hooks& h);

        TaskHandle_t task();
    }
}
