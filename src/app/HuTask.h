#pragma once
#include <Arduino.h>
#include "../proto/canbox/Protocol.h"
#include "../vehicle/VehicleState.h"
#include "../utils/Staged.h"

class UartTx;

// The head-unit link (docs/design/event-tasks.md, Step 2). Priority 6 on core 1,
// woken by its out-queue or every 2 ms, which is also its RX poll: at 38400
// baud a byte takes 260 us, so 2 ms is eight bytes.
//
// It owns Serial1 RX (the parser, and the ack the moment a frame ends), every
// non-key byte written to Serial1 (0x90 replies, vehicle telemetry, raw frames
// from the web and macros), the protocol's emitter and its schedule, and the HU frame
// ring. Keys are the keys task's and go straight to UartTx; nothing else on any
// other task writes the port, and nothing off core 1 ever takes its mutex.
//
// Other tasks post frames (post()), stage emitter settings (emitterCfg()), or
// ask for a message to be sent (requestSendNow(), sendNowWait()).
namespace App
{
    namespace Hu
    {
        // What a queued frame is, for the lane and the drop counters.
        enum class Kind : uint8_t { Vehicle = 0, Raw = 1, COUNT = 2 };

        struct Setup {
            HardwareSerial*      port    = nullptr;
            UartTx*              tx      = nullptr;
            Canbox::Decoder*     rx      = nullptr;   // the protocol's, inbound
            // The head-unit protocol and its live emitter (the hu task's own).
            const Canbox::Protocol* proto   = nullptr;
            Canbox::Emitter*        emitter = nullptr;
            Vehicle::State*      state   = nullptr;
            // Once per pass, on the hu task, after the RX drain: the owner's
            // own housekeeping (junk batching, the ring's serial echo).
            void (*wake)(uint32_t nowMs) = nullptr;
        };

        // Before any producer: creates the queues and the emitter's staging,
        // seeded with `cfg`. start() creates the task.
        // `cfg` is the emitter's settings as the web edits them (Canbox::HuConfig;
        // its field map is NVS "huFmap"); the hu task applies each edit.
        // `inbound`: what the head unit's frames mean, for the caller (its
        // ackDue is the hu task's own and is replaced).
        void begin(const Setup& s, const Canbox::HuConfig& cfg, const Canbox::HuInbound& inbound);
        void start();
        // The head unit's ACK (0xFF) of one of our frames; any task.
        void noteAckIn();

        // Any task, zero wait. A frame up to Canbox::HU_FRAME_MAX bytes, written
        // through the vehicle lane in order. False = queue full, counted.
        bool post(Kind k, const uint8_t* frame, uint8_t len);

        // ---- the emitter, from other tasks ---------------------------------
        Staged<Canbox::HuConfig>& emitterCfg();
        // The live emitter's counters, any task (plain reads); null before begin.
        const Canbox::Emitter* emitter();
        // Macros: send message `idx` once, at the hu task's next pass. Queued
        // (a few deep, so macros fired together all send), counted when full.
        void requestSendNow(uint8_t idx);
        // httpd: the same, waiting up to 2 s for the answer.
        enum class SendNow : uint8_t { Sent, NoData, Busy };
        SendNow sendNowWait(uint8_t idx);
        // httpd: the frame message `idx` would carry now, built from the
        // staged settings on the caller's stack, so nothing the hu task is
        // using is read mid-change.
        bool preview(uint8_t idx, uint8_t* frame, uint8_t& len);

        // ---- the hu task only ------------------------------------------------
        // The 0x90 responder, called from the frame callback: the reply goes in
        // the reply lane, ahead of queued telemetry.
        bool sendByCmd(uint8_t cmd);
    }
}
