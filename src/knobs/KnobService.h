#pragma once
#include "../Features.h"
#if FEATURE_KNOBS
#include <Arduino.h>
#include "KnobConfig.h"
#include "KnobSource.h"
#include "KnobLights.h"
#include "LedStrip.h"
#include "../keys/KeyAction.h"

// Hardware half of the knobs: pins, the 1 kHz task, LED strip.
//
// Threading:
//   knob task  reads pins, runs Source and Lights, drives the strip, and hands
//              each event to the post function (the KeyBus): zero wait, so a
//              busy keys task can never cost the sampler a detent.
//   web task   config()/setConfig()/live(): swaps Config under a mutex and sets
//              a flag; never touches Source or Lights. The knob task only
//              try-takes that mutex and keeps its last Config while it is held.
namespace Knobs
{
    class Service {
    public:
        struct Live {
            uint32_t encDetents, encErrors, clicks, holds, potSteps, dropped, frames, busy;
            uint32_t enc2Detents, enc2Errors, clicks2, holds2;
            uint16_t potRaw, mA, mAWanted, leds;
            uint8_t  potLevel, potZone, master;
            bool     btn, btn2, stripOk;
            // Boards whose knobs sit behind a PCF8574 (Board::KNOBS_I2C): its
            // address, 0 = not found; failed reads since boot.
            uint8_t  expAddr;
            uint32_t expErrors;
            // The six lines as last sampled, before any decoding: bit 0-2 =
            // right A/B/key, 3-5 = left; set = closed (pulled to ground).
            uint8_t  lines;
            // Changes of each of those six lines since boot, counted at the
            // sampling rate: a slow poll still sees every flicker.
            uint16_t edges[6];
            // Signed detents since boot (right, left): + = the cw action
            // fired, after `inverted`. Says which way, where detents cannot.
            int32_t  pos[2];
        };

        // Where events go. `pair` = a press whose release follows at once (a
        // click): post it as ONE item, so a full queue drops both or neither.
        // False = not taken (counted in Live::dropped).
        using PostFn = bool (*)(const Keys::Event& e, bool pair, void* ctx);

        void begin(const Config& c, PostFn post, void* ctx);

        Config config();
        void   setConfig(const Config& c);
        Live   live();

    private:
        static void taskThunk(void* self);
        void        run();

        // The six knob lines, active (closed contact) = true, from GPIOs or the
        // expander. False when the expander read failed: skip this sample.
        struct Lines { bool a1, b1, k1, a2, b2, k2; };
        bool        readLines(Lines& l, uint32_t nowMs);
        // The PCF8574, by the IDF driver directly (no Arduino log_e per failed
        // transfer, see KnobService.cpp). probe(): 1 answered, 0 no ACK,
        // -1 timed out (SDA or SCL held low: the bus is stuck).
        int8_t      probe(uint8_t addr);
        bool        findExpander();                 // whole sweep: setup only
        void        searchStep(uint32_t nowMs);     // one address per tick
        bool        logDue(uint32_t nowMs);
        bool              _i2cOk = false;           // Wire.begin() succeeded
        uint8_t           _expAddr = 0;
        volatile uint32_t _expErrors = 0;           // failed reads since boot
        volatile uint8_t  _lines = 0;               // Live::lines
        volatile uint16_t _edges[6] = {};           // Live::edges
        volatile int32_t  _pos[2] = {};             // Live::pos
        uint8_t           _expMiss = 0;             // failed reads in a row
        uint8_t           _probeIdx = 0;            // next of the 16 addresses
        bool              _busStuck = false;
        uint16_t          _expBackoffMs = 1000;
        uint32_t          _expRetryAt = 0;
        uint32_t          _logAt = 0;
        uint16_t          _logSkipped = 0;
        static void emitThunk(const Keys::Event& e, bool pair, void* self);

        Config            _cfg;
        volatile bool     _dirty = false;
        SemaphoreHandle_t _lock = nullptr;
        PostFn            _post = nullptr;
        void*             _postCtx = nullptr;
        TaskHandle_t      _task = nullptr;
        int               _regId = -1;      // App::Tasks entry; written before the task starts
    public:
        // Knob task stack high-water mark (the [stk] line in main.cpp).
        uint32_t stackFree() const { return _task ? (uint32_t)uxTaskGetStackHighWaterMark(_task) : 0u; }
    private:
        uint32_t          _dropped = 0;

        Source   _src;
        Lights   _lights;
        LedStrip _strip;
    };
}
#endif
