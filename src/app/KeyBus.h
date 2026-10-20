#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_timer.h>
#include "../keys/KeyRouter.h"

// Every key producer's one way in to the keys task (docs/design/event-tasks.md
// §3.2): the panel's key callback, the knobs, the matrix scan, httpd and the
// macro runner post here, with zero wait; only the keys task takes. The lane
// rings and the order they are taken in are Keys::KeyQueues (portable, tested);
// this is that object under a portMUX, plus the wake-up.
//
// A full lane drops the message and counts it. Nothing here ever blocks, so a
// producer that is itself on a deadline (the panel inside poll(), the 1 kHz
// knob sampler) can call it.
namespace App
{
    namespace KeyBus
    {
        // The keys task, once it exists. Posts before that are queued and
        // handled on its first pass.
        void setConsumer(TaskHandle_t t);

        // Any task, never an ISR. False = dropped (counted per lane).
        bool post(Keys::Lane lane, const Keys::KeyMsg& m);

        // The source clock the latency histogram measures from.
        inline uint32_t nowUs() { return (uint32_t)esp_timer_get_time(); }

        // One event as its source produced it: a Release goes on the Rel lane,
        // anything else on the Tap lane.
        bool postEvent(const Keys::Event& e);
        // A press and its release as one item (a knob click), on `lane`.
        bool postTap(const Keys::Event& e, Keys::Lane lane = Keys::Lane::Tap);
        // The panel's raw key, mapped on the keys task. pollGapUs: time since
        // the previous poll() started (how long the frame could have waited).
        bool postPanelKey(uint16_t code, bool hold, uint32_t tMs, uint32_t pollGapUs);
        // Web: a literal RAV4 code, optionally with its release.
        bool postLiteral(uint8_t code, uint8_t state, bool release);
        // Web: perform macro row `idx`.
        bool postMacroRun(uint8_t idx);

        // Wake the keys task with nothing queued: a staged config changed and
        // should apply now rather than at its next deadline.
        void kick();

        // ---- the keys task only ------------------------------------------
        bool    pop(Keys::KeyMsg& out);
        bool    empty();
        // Lost releases that may be synthesised now (KeyQueues::takeLostReady).
        uint8_t takeLostReady(Keys::Event* out, uint8_t max, bool& releaseAll);

        // ---- readers (the heartbeat) ---------------------------------------
        uint32_t drops(Keys::Lane l);
        uint32_t tapDropsFrom(uint8_t srcId);
    }
}
