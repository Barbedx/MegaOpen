#pragma once
#include "KnobConfig.h"

// What the LEDs behind the two knobs show. Portable: fills a buffer of RGB
// triplets and reports changes; LedStrip puts it on the wire.
//
// Two layers per zone:
//   RESTING   per-LED colours, optionally animated (Static, Breathe, Rainbow).
//   REACTION  on top, for reactHoldMs after the last movement (see React).
//             Level on an encoder runs over a virtual position, levelSteps
//             clicks long; it does not know the head unit's volume.
//
// Every LED eases towards its target over a few frames; nothing snaps.
namespace Knobs
{
    class Lights {
    public:
        void configure(const Config& c);

        // `detents` clicks on the zone's knob (encoder).
        void onTurn(uint8_t zone, int8_t detents, uint32_t nowMs);
        // The zone's knob is at `level` 0..255 (pot). The first call is a
        // position, not a movement.
        void setLevel(uint8_t zone, uint8_t level, uint32_t nowMs);
        void onButton(uint8_t zone, uint32_t nowMs);

        // Overrides Config::brightness — the pot as a dimmer.
        void setMaster(uint8_t brightness) { _master = brightness; }
        uint8_t master() const { return _master; }

        // One frame (call at ~50 Hz). True when the pixels differ from the last.
        bool tick(uint32_t nowMs);

        // RGB triplets in chain order, gamma and brightness already applied.
        const uint8_t* pixels() const { return _px; }
        uint8_t        count()  const { return _count; }

        uint8_t level(uint8_t zone) const { return zone < ZONES ? _z[zone].level : 0; }
        uint8_t head(uint8_t zone)  const { return zone < ZONES ? _z[zone].head : 0; }

        // ---- the supply -----------------------------------------------------
        // Estimated draw of the last frame after the limiter, and what it would
        // have drawn without it.
        uint16_t milliamps()       const { return _mA; }
        uint16_t milliampsWanted() const { return _mAWanted; }
        bool     limited()         const { return _mAWanted > _mA; }

        // Exposed for the tests.
        static uint32_t estimateMilliamps(const uint8_t* rgb, uint8_t count);

    private:
        struct ZoneState {
            uint8_t  level = 0;
            int16_t  virt = 0;          // the encoder's virtual position, in clicks
            uint8_t  head = 0;          // Spin / Trail: which LED is lit
            uint32_t sweepAt = 0;       // Sweep: when the last click's dot set off
            int8_t   sweepDir = 1;      // ...and which way round
            bool     seeded = false;
            bool     levelSeeded = false;   // setLevel(): the first reading is a position
            bool     moved = false;
            uint32_t lastMoveMs = 0;
            uint8_t  flash = 0;
            uint16_t cur[ZONE_LEDS][3] = {};    // 8.8, what is on the glass now
            bool     curSeeded = false;
        };

        void touch(uint8_t zone, uint32_t nowMs);
        bool live(uint8_t zone, uint32_t nowMs) const;
        Rgb  resting(uint8_t zone, uint8_t i, uint32_t nowMs) const;
        Rgb  target(uint8_t zone, uint8_t i, uint32_t nowMs) const;

        void renderFeet(uint8_t* out, uint32_t nowMs);
        void limitCurrent(uint8_t* rgb);

        Config    _c;
        ZoneState _z[ZONES];
        uint8_t   _master = 128;
        uint8_t   _px[MAX_LEDS * 3] = {0};
        // The frame being built. A member, not a local: ~0.5 kB on a task with
        // a 4 kB stack.
        uint8_t   _nx[MAX_LEDS * 3] = {0};
        uint8_t   _count = 0;

        // Footwells ease as one colour and one level; the level starts at zero.
        uint16_t  _feetCur[3] = {0, 0, 0};      // 8.8
        uint16_t  _feetLevel = 0;               // 8.8
        uint16_t  _mA = 0, _mAWanted = 0;
    };
}
