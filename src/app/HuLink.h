#pragma once
#include <Arduino.h>
#include "../cfg/Config.h"
#include "../net/Catalog.h"
#include "../proto/canbox/Protocol.h"
#include "../vehicle/VehicleState.h"

class UartTx;

// What the hu task (App::Hu, HuTask.h) is wired to: Serial1 and UartTx on it,
// the protocol's emitter and decoder (made once, in internal RAM), what the
// head unit's frames mean (the clock, the 0x90 requests, the track text), and
// the ring of recent frames both ways with its [hu] echo at TRC. HuTask stays
// the generic engine; this is the head unit as this firmware sees it.
namespace App
{
    namespace HuLink
    {
        // Serial1 at the saved baud, and UartTx on it, every frame into the ring.
        void beginUart(Cfg::Config& cfg);
        // The emitter, the decoder and the hu task's setup (App::Hu::begin).
        void begin(Cfg::Config& cfg, Vehicle::State& state);

        UartTx& tx();
        // Null when there was no memory for it (logged at boot).
        Canbox::Emitter* emitter();
        Canbox::Decoder* decoder();

        // /api/canbox/last: newest first; `inOnly` = the head unit's frames alone.
        void lastFramesJson(bool inOnly, Catalog::Out& out);
    }
}
