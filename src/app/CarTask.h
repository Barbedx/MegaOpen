#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../bus/IVehicleSource.h"
#include "../vehicle/VehicleState.h"
#include "../vehicle/CanSniffer.h"
#include "../vehicle/DecodeTable.h"
#include "../vehicle/AlertRules.h"
#include "../utils/Staged.h"

// The car task (docs/design/event-tasks.md, Step 5). Priority 4 on core 0 for
// now: the core is decided by measurement (TaskPlan.h, CAR_CORE).
//
// It owns the vehicle sources and everything behind them (the MCP2518FD's SPI),
// the decode into Vehicle::State, the sniffer table and the
// capture hook. Per frame: CanDecode, the main.cpp tap (raw ring, counters,
// the web's raw stream), the sniffer when it is on, the recorder.
//
// WAKES on a task notify from the MCP2518FD's INT (T-2CAN), or every 2 ms when
// a polled source is in the list (the TWAI on env:direct), else every
// 20 ms. Each wake drains up to 64 frames per source; a source that filled
// them gets the CPU back after a yield, not after the next wake.
//
// THE ONLY WRITER OF Vehicle::State. Decoded values are written here; the web's
// pins come here as commands (pin()/unpin()) and are applied between two
// frames. Readers (the hu task's emitter, httpd) read single aligned words, so
// a reader can see one signal's value a frame older than its neighbour's but
// never a torn number. What is still not here: the Rav4 emitter stays on the
// hu task (HuTask.h), reading State the same way; moving it and the 0x90 reply
// path is a later step.
//
// THE DECODE MAP is its too: the NVS blob `decode` (else decode_defaults.inc)
// is loaded at begin(); the web stages edits (decodeTable()) and the task
// applies them between two frames, no reboot. A constant binding is a pin it
// keeps. Its two buffers are in PSRAM where there is some.
//
// Not created when the list is empty: the loop then
// calls service(), which does the task's command and staging work, so State
// still has one writer.
namespace App
{
    namespace Car
    {
        constexpr uint8_t MAX_SOURCES = 3;
        constexpr size_t  DRAIN_MAX   = 64;

        struct Setup {
            IVehicleSource*      src[MAX_SOURCES] = {};
            uint8_t              n = 0;
            Vehicle::State*      state = nullptr;
            // Null = no table in this image.
            Vehicle::CanSniffer* sniffer = nullptr;
            // Every frame, on the car task, after the decode: main.cpp's frame
            // ring, its counter and the web's raw stream. Must not block.
            void (*tap)(const Frame& f) = nullptr;
            // The decode map as NVS holds it (Config::loadDecode); none, or one
            // that does not unpack, is the factory table.
            const uint8_t*       decodeBlob = nullptr;
            size_t               decodeLen = 0;
            // The panel alert rules (NVS "alerts", else factory: the low-fuel
            // edge). Copied.
            Vehicle::AlertRules  alerts;
            // A rule fired: put it on the panel (the ui task's notification).
            // Null = no panel in this image: rules are evaluated, nothing shown.
            void (*alert)(const char* title, const char* text) = nullptr;
            // Whether a popup may go up now: false while a menu or a person's
            // screen holds the glass. Null = always.
            bool (*glassFree)() = nullptr;
        };

        // setup(), before start(): the command queue and the decode table.
        void begin(const Setup& s);
        // setup(): creates the task when the list has a source. The sources'
        // begin() runs on the task, which is what puts their hardware on it.
        void start();
        bool         running();
        TaskHandle_t task();

        // The loop, when no task was created: commands, staging, test expiry.
        void service(uint32_t nowMs);

        // ---- any task: State's pins, applied by the car task ------------------
        // Zero wait; a full queue drops the edit and counts it.
        void pin(uint8_t sig, int32_t value);
        void unpin(uint8_t sig);
        // /api/map/test: pin `value` for `forMs`, then give the signal back to
        // its binding (CAN, or its constant). RAM only.
        void test(uint8_t sig, int32_t value, uint32_t forMs);
        // Any task: what is left of a test on `sig`, 0 = none.
        uint32_t testLeftMs(uint8_t sig, uint32_t nowMs);

        // ---- the decode map ------------------------------------------------------
        // httpd edits it (stage/edit); the car task applies it.
        // Persisting it is the caller's (pack() and Config::saveDecode).
        Staged<Vehicle::DecodeTable, true>& decodeTable();

        // ---- the panel alert rules ---------------------------------------------------
        // httpd edits (and persists, Config::saveAlerts); the car task applies
        // them between two frames. Evaluated every ALERT_MS against State.
        constexpr uint32_t ALERT_MS = 100;
        Staged<Vehicle::AlertRules>& alertRules();
        struct AlertStats { uint32_t fired = 0, shown = 0, overwritten = 0, dropped = 0; };
        AlertStats alertStats();

        struct Stats {
            uint32_t frames = 0;        // since boot, all sources
            uint32_t fps = 0;           // the last second
            uint32_t drainMax = 0;      // most frames in one wake, this window
            uint32_t decodeHits = 0;    // frames that matched a decode row
            uint32_t decodeUnknown = 0; // frames that matched none
            uint32_t decodeShort = 0;   // rows skipped: frame too short
            uint32_t cmdDrops = 0;      // pins the queue had no room for
            uint32_t busyMaxUs = 0;     // longest wake, this window
            uint32_t srcDropped = 0;    // the sources' own losses, summed
            uint8_t  sources = 0;
        };
        // Published about 4 times a second; any task reads a copy.
        Stats stats();
    }
}
