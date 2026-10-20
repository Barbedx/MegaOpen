#pragma once
#include <Arduino.h>
#include "../keys/KeyRouter.h"
#include "../keys/KeyMap.h"
#include "../input/KeyDebouncer.h"

class UartTx;

// The one task that turns keys into output (docs/design/event-tasks.md, Step 2).
// Priority 7 on core 1, above everything that renders: a key never waits for
// a panel repaint, a BLE write, a web request or a telemetry frame.
//
// It owns KeyEngine dispatch, MacroEngine, the panel key mapping, the UART key
// sink and its release table, USB HID and the resistive ladder. Everyone else
// posts to the KeyBus (src/app/KeyBus.h), and the web stages its edits
// (Staged<T>, applied here with a try-take, never a wait).
//
// One pass: apply staged configs, take what is queued (release lane first),
// synthesise any release the bus had to drop, then run every sink deadline.
// It sleeps until the next deadline exactly, or 100 ms, or the next post.
namespace App
{
    namespace KeysTask
    {
        // Where each class of key goes, as the web edits it.
        struct Routing {
            bool     uartOn  = true;
            uint8_t  uartCls = 0x0F;     // Keys::CLASS_ALL
            uint16_t pulseMs = 60;
            bool     resOn   = false;
            uint8_t  resCls  = 0;
            uint8_t  amsCls  = 0;
            uint8_t  hidCls  = 0;
            uint8_t  usbCls  = 0;        // FEATURE_USB_HID only
        };

        // A learn hold on the resistive ladder, asked by the web. The keys task
        // drives the rungs, so the page only posts this; it has already checked
        // the rung exists, so it can answer 409 itself.
        struct ResLearn {
            enum class Op : uint8_t { Action, Assert };
            Op       op     = Op::Action;
            uint8_t  action = 0;      // Keys::Action, for Op::Action
            uint8_t  ch     = 0;      // for Op::Assert
            uint8_t  mask   = 0;
            uint32_t ms     = 0;
        };

        // The matrix's map and timing. Applied by the task that sweeps the
        // matrix (the scan task), since that is where they are read.
        struct KeyMapCfg {
            Keys::KeyMap                 map;
            bool                         encInverted = false;
            Input::KeyDebouncer::Params  timing;
        };

        struct Hooks {
            // Staged configs into the objects this task owns.
            void (*apply)() = nullptr;
            // Every sink deadline: releases, ladder steps, macro windows.
            void (*deadlines)(uint32_t nowMs) = nullptr;
            // The earliest of them; false when nothing is pending.
            bool (*nextDue)(uint32_t nowMs, uint32_t& dueMs) = nullptr;
            // What each message kind expands into.
            Keys::KeyRouter::Hooks route;
        };

        // Creates the task. Call once every sink is registered: the first
        // post may be handled before begin() returns.
        void begin(const Hooks& h, UartTx* tx);

        // The engine's event hook, on this task: keeps the down set honest.
        void observe(const Keys::Event& e);
    }
}
