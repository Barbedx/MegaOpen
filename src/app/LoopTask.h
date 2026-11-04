#pragma once
#include "../Features.h"
#include <Arduino.h>
#include "../bus/Frame.h"
#include "../cfg/Config.h"
#include "../vehicle/CanSniffer.h"
#if FEATURE_DISPLAY_CAN
#include "../display/PanelBusWatch.h"
#endif

// The loop task: the Arduino loop, which is the panel task and the TWAI
// driver's owner (TaskPlan.h: priority 5, core 1). It holds the TWAI link and
// what rides on it by build: the panel (the display's construction, the raw
// tap and the AFFA frame trace, who else is on the bus) where there is one,
// else the car's bus, listen-only, drained to the car task (env:direct). Each
// pass pumps the panel (App::Panel, PanelOwner.h), samples the controller
// (Diag::CanBench) and writes this task's own [can], [panel], [stall] and
// [loop] lines; between passes it sleeps in the link's waitRx().
namespace App
{
    namespace Loop
    {
        // setup(): this task in the registry, critical (src/app/TaskRegistry.h).
        void registerTask();
        // setup(): the TWAI and what it carries, then the controller sample.
        // `sniffer`: the per-ID table, fed from the panel bus while no car
        // task owns it; null without one.
        void begin(Cfg::Config& config, Vehicle::CanSniffer* sniffer);
        // A beat now: net_svc checks every critical task's age from its first pass.
        void beat();
        // The end of setup(): this task's priority, and a beat.
        void start();
        // loop(): one pass, then the sleep until the next frame or 2 ms.
        void pass();

        TaskHandle_t task();
        // Frames seen on the TWAI and the car's bus, since boot.
        const uint32_t* canRx();
#if FEATURE_DISPLAY_CAN || FEATURE_VEHICLE_CAN
        // Every car frame, on the car task (App::Car's tap), after the decode.
        void carTap(const Frame& f);
#endif
#if FEATURE_DISPLAY_CAN
        // Who else is on the panel bus; /api/health reads it.
        const MegaOpen::PanelBusWatch& panelWatch();
#endif
    }
}
