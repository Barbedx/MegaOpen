#include "HuTask.h"
#include "../utils/Scratch.h"
#include "Rt.h"
#include "RequestReply.h"
#include "../sinks/UartTx.h"
#include "../sinks/UartLanes.h"
#include "../utils/LogBus.h"
#include "freertos/queue.h"
#include <atomic>
#include <string.h>

namespace App
{
    namespace Hu
    {
        // ACKs (0xFF) from the head unit, counted by the parser's junk hook.
        static std::atomic<uint32_t> s_acksIn{0};
        void noteAckIn() { s_acksIn.fetch_add(1, std::memory_order_relaxed); }

        namespace
        {
            // 38400 baud is 3.8 bytes a millisecond; 512 is two passes of a
            // backlog after a stall, and bounds how long one pass can read.
            constexpr size_t   RX_MAX_PER_WAKE = 512;
            constexpr uint8_t  OUT_DEPTH       = 16;
            constexpr uint8_t  RR_DEPTH        = 4;
            constexpr uint8_t  SEND_NOW_DEPTH  = 4;
            // The head unit polls a dozen or so ids at start-up, back to back;
            // each answer waits for an empty FIFO, so they queue here.
            constexpr uint8_t  REPLY_DEPTH     = 8;
            // A long frame (raw, over UartLanes::QUIET_LEN_MAX) waits for the
            // head unit to go quiet. One that has not gone out by then is
            // dropped and counted, so it cannot hold the vehicle lane for ever
            // on a chatty head unit.
            constexpr uint32_t PENDING_MAX_MS  = 500;
            // Silence inside a frame longer than this abandons it: the head
            // unit reset mid-frame or the length byte was corrupt
            // (RxParser::reset). A unit writes a frame in one go, and at
            // 38400 the core hands bytes over one at a time (RX FIFO-full
            // threshold 1 below 57600 baud), so 10 ms is ~38 byte times of
            // real silence; the frame's ack window is gone by then anyway.
            constexpr uint32_t RX_GAP_MS       = 10;
            constexpr uint32_t LOG_MS          = 10000;   // the [hu] line
            constexpr uint8_t  OP_SEND_NOW     = 1;

            struct OutFrame {
                uint8_t kind = 0;
                uint8_t len  = 0;
                uint8_t b[Canbox::MAX_FRAME];
            };

            Setup              s_s;
            // Static queues: created in begin() without the heap, so they
            // cannot fail and leave the task waiting on a null handle.
            StaticQueue_t      s_outBuf, s_rrBuf, s_sendNowBuf;
            uint8_t            s_outStore[OUT_DEPTH * sizeof(OutFrame)];
            uint8_t            s_rrStore[RR_DEPTH * sizeof(RR::Ticket)];
            uint8_t            s_sendNowStore[SEND_NOW_DEPTH];
            QueueHandle_t      s_out = nullptr;
            QueueHandle_t      s_rrQ = nullptr;
            // Macro Message effects: a queue, not a latest-wins slot, so two
            // macros fired close together both send.
            QueueHandle_t      s_sendNowQ = nullptr;
            std::atomic<uint32_t> s_sendNowDrops{0};
            RR::Channel        s_rr;
            Staged<EmitterCfg> s_cfg;
            int                s_id = -1;

            // The frame at the head of the vehicle lane, kept across passes
            // while the gate says "not yet". Hu task only.
            OutFrame s_pending;
            bool     s_havePending = false;
            uint32_t s_pendingSinceMs = 0;
            // 0x90 answers, ahead of the vehicle lane, oldest first, each
            // retried until the gate lets it out.
            OutFrame s_reply[REPLY_DEPTH];
            uint8_t  s_replyHead = 0, s_replyCount = 0;
            bool     s_replyMode = false;

            // RX timing, for the lane gate and the ack measurement.
            uint32_t s_lastRxMs = 0;
            uint32_t s_readAtUs = 0;

            // This window's evidence. The drops are bumped by producers on any
            // task: atomic. The rest is the hu task's own.
            std::atomic<uint32_t> s_outDrops[(uint8_t)Kind::COUNT];
            uint32_t s_ackLateMaxUs = 0;
            uint32_t s_lmax = 0;
            uint32_t s_replyRetries = 0, s_replyLost = 0;
            uint32_t s_rxBytes = 0;
            uint32_t s_pendingStale = 0;

            void applyCfg(const EmitterCfg& c)
            {
                s_s.emitter->setEnabled(c.enabled);
                s_s.emitter->setTuning(c.tuning);
                s_s.emitter->setFieldMap(c.fields);
                for (uint8_t i = 0; i < Canbox::Rav4Emitter::MAX_MESSAGES; ++i)
                    s_s.emitter->setMsgEnabled(i, c.msgOn[i]);
            }

            // The emitter's write: into the reply queue while answering a 0x90,
            // otherwise onto the vehicle lane behind whatever is queued.
            void emitWrite(const uint8_t* f, uint8_t len, void*)
            {
                if (s_replyMode) {
                    // Full: this answer is the one lost; the head unit asks
                    // again on its next poll.
                    if (s_replyCount >= REPLY_DEPTH || len > Canbox::MAX_FRAME) { ++s_replyLost; return; }
                    OutFrame& r = s_reply[(uint8_t)((s_replyHead + s_replyCount) % REPLY_DEPTH)];
                    r.kind = (uint8_t)Kind::Vehicle;
                    r.len  = len;
                    memcpy(r.b, f, len);
                    ++s_replyCount;
                    return;
                }
                post(Kind::Vehicle, f, len);
            }

            // The ack the moment a frame ends, before it is even checked or
            // parsed (RxParser::onAck). Late = how long since this read began
            // plus the bytes it found in the FIFO: the part the 2 ms poll adds
            // before the read is not visible from here.
            void ackNow(void*)
            {
                uint16_t inflight = 0;
                if (!s_s.tx->writeAck(&inflight)) return;
                const uint32_t late = (micros() - s_readAtUs) + Sinks::UartLanes::wireUs(inflight);
                if (late > s_ackLateMaxUs) s_ackLateMaxUs = late;
            }

            // Bounded, and never a blocking read: read(buf, n) uses timeout 0,
            // and only what available() reported is asked for. readBytes() would
            // wait on the HAL mutex the key writes share (tools/check_rules.py).
            void rxDrain(uint32_t now)
            {
                uint8_t buf[64];
                size_t budget = RX_MAX_PER_WAKE;
                s_readAtUs = micros();
                while (budget) {
                    const int avail = s_s.port->available();
                    if (avail <= 0) break;
                    size_t n = (size_t)avail;
                    if (n > sizeof(buf)) n = sizeof(buf);
                    if (n > budget) n = budget;
                    const size_t got = s_s.port->read(buf, n);
                    if (!got) break;
                    s_lastRxMs = now;
                    s_rxBytes += got;
                    for (size_t i = 0; i < got; ++i) s_s.rx->feed(buf[i]);
                    budget -= got;
                }
            }

            void serveRequests()
            {
                RR::Ticket t;
                while (s_rrQ && xQueueReceive(s_rrQ, &t, 0) == pdTRUE) {
                    RR::Args a;
                    if (!s_rr.take(t, a)) continue;          // the waiter gave up
                    RR::Result r;
                    if (a.op == OP_SEND_NOW)
                        r.status = s_s.emitter->sendNow((uint8_t)a.u[0], *s_s.state) ? 1 : 0;
                    s_rr.complete(t, r);
                }
                uint8_t idx;
                while (s_sendNowQ && xQueueReceive(s_sendNowQ, &idx, 0) == pdTRUE)
                    if (!s_s.emitter->sendNow(idx, *s_s.state))
                        LOGI("macro", "message %u has no data - pin its signals", (unsigned)idx);
            }

            void track(uint8_t len) { if (len > s_lmax) s_lmax = len; }

            // One non-key frame per pass at most: the gate wants an empty FIFO,
            // and the frame just written fills it for a few milliseconds.
            void writeOut(uint32_t now)
            {
                const bool     mid   = s_s.rx->midFrame();
                const uint32_t quiet = now - s_lastRxMs;
                using Lane = Sinks::UartLanes::Lane;

                if (s_replyCount) {
                    const OutFrame& r = s_reply[s_replyHead];
                    const UartTx::Wrote w = s_s.tx->writeLane(Lane::Reply, r.b, r.len, mid, quiet, 'V');
                    if (w == UartTx::Wrote::Deferred) { ++s_replyRetries; return; }
                    if (w == UartTx::Wrote::Written) track(r.len);
                    s_replyHead = (uint8_t)((s_replyHead + 1) % REPLY_DEPTH);
                    --s_replyCount;
                    return;
                }
                if (!s_havePending) return;
                const char dir = s_pending.kind == (uint8_t)Kind::Raw ? 'T' : 'V';
                const UartTx::Wrote w = s_s.tx->writeLane(Lane::Vehicle, s_pending.b, s_pending.len,
                                                          mid, quiet, dir);
                if (w == UartTx::Wrote::Deferred) {
                    // Only a long frame can wait for ever (the quiet rule); a
                    // short one goes at the next empty FIFO.
                    if (s_pending.len <= Sinks::UartLanes::QUIET_LEN_MAX ||
                        (uint32_t)(now - s_pendingSinceMs) < PENDING_MAX_MS) return;
                    ++s_pendingStale;
                }
                s_havePending = false;
                if (w == UartTx::Wrote::Written) track(s_pending.len);
            }

            void logWindow()
            {
                UartTx& tx = *s_s.tx;
                // *Dropped = the UART mutex was not free within 20 ms: the frame
                // is gone, and it should stay 0. vehDeferred = passes where the
                // lane gate held a frame back for an ack or a key: the design
                // working. Lmax = the longest non-key frame written this window.
                // stale = a frame that found no gap in PENDING_MAX_MS and was
                // dropped; msgDrops = macro Messages the queue had no room for.
                // desync = frames abandoned after RX_GAP_MS of silence mid-frame.
                LOGS("hu", "acks=%u acksIn=%u ackLateMax=%uus keys=%u veh=%u replies=%u keyDropped=%u "
                               "ackDropped=%u vehDropped=%u vehDeferred=%u outDrops(veh/raw)=%u/%u "
                               "replyRetries=%u replyLost=%u stale=%u msgDrops=%u Lmax=%u rx=%u desync=%u",
                               (unsigned)tx.acks(), (unsigned)s_acksIn.load(), (unsigned)s_ackLateMaxUs, (unsigned)tx.keyFrames(),
                               (unsigned)tx.vehicleFrames(), (unsigned)tx.replyFrames(),
                               (unsigned)tx.keyDropped(), (unsigned)tx.ackDropped(),
                               (unsigned)tx.vehDropped(), (unsigned)tx.vehicleDeferred(),
                               (unsigned)s_outDrops[0].load(), (unsigned)s_outDrops[1].load(),
                               (unsigned)s_replyRetries, (unsigned)s_replyLost,
                               (unsigned)s_pendingStale, (unsigned)s_sendNowDrops.load(),
                               (unsigned)s_lmax, (unsigned)s_rxBytes, (unsigned)s_s.rx->desyncs());
                s_ackLateMaxUs = 0;
                s_lmax = 0;
            }

            void run(void*)
            {
                uint32_t logAt = millis();
                for (;;) {
                    // Woken by a queued frame or after 2 ticks, which is the RX
                    // poll. With a frame already held back by the gate, the
                    // queue is not waited on (it would return at once): a
                    // tick's sleep, then the gate is asked again.
                    if (s_havePending || s_replyCount) {
                        vTaskDelay(1);
                    } else if (xQueueReceive(s_out, &s_pending, 2) == pdTRUE) {
                        s_havePending = true;
                        s_pendingSinceMs = millis();
                    }

                    const uint32_t t0 = micros();
                    const uint32_t now = millis();
                    rxDrain(now);
                    // A frame that stopped half way would otherwise swallow
                    // the next ones and keep the lanes shut.
                    if (s_s.rx->midFrame() && (uint32_t)(now - s_lastRxMs) > RX_GAP_MS)
                        s_s.rx->reset();
                    if (s_cfg.applyIfDirty()) applyCfg(s_cfg.live());
                    serveRequests();
                    s_s.emitter->tick(*s_s.state, now);
                    if (s_s.wake) s_s.wake(now);
                    // Read again right before the gate: a frame that began
                    // while the pass formatted (wake's echo lines) is then
                    // seen as mid-frame, and acked, before anything is written.
                    const uint32_t outNow = millis();
                    rxDrain(outNow);
                    writeOut(outNow);

                    if ((uint32_t)(now - logAt) >= LOG_MS) {
                        logAt = now;
                        logWindow();
                    }
                    Tasks::beat(s_id, micros() - t0);
                }
            }

            bool postTicket(const RR::Ticket& t, void*)
            {
                return s_rrQ && xQueueSend(s_rrQ, &t, 0) == pdTRUE;
            }
        }

        void begin(const Setup& s, const EmitterCfg& cfg)
        {
            s_s = s;
            for (auto& d : s_outDrops) d.store(0);
            s_out = xQueueCreateStatic(OUT_DEPTH, sizeof(OutFrame), s_outStore, &s_outBuf);
            s_rrQ = xQueueCreateStatic(RR_DEPTH, sizeof(RR::Ticket), s_rrStore, &s_rrBuf);
            s_sendNowQ = xQueueCreateStatic(SEND_NOW_DEPTH, 1, s_sendNowStore, &s_sendNowBuf);
            s_rr.begin();
            s_cfg.init(cfg);
            applyCfg(cfg);
            s_s.emitter->begin(emitWrite, nullptr);
            s_s.rx->onAck(ackNow, nullptr);
            // Nothing here reads with a timeout; this makes any Stream helper
            // that would (readBytes, parseInt) return at once instead of
            // holding the HAL mutex the key writes share (§3.3).
            s_s.port->setTimeout(0);
        }

        void start()
        {
            // Critical: a dead hu task is no acks, and a head unit that resends
            // every frame for ever.
            TaskSpec spec;
            spec.name     = "hu";
            spec.fn       = run;
            spec.stack    = Plan::HU_STACK;
            spec.prio     = Plan::HU_PRIO;
            spec.core     = Plan::HU_CORE;
            spec.critical = true;
            spec.idOut    = &s_id;
            spawn(spec);
        }

        bool post(Kind k, const uint8_t* frame, uint8_t len)
        {
            const uint8_t ki = (uint8_t)k < (uint8_t)Kind::COUNT ? (uint8_t)k : 0;
            if (!s_out || !frame || !len || len > Canbox::MAX_FRAME) {
                s_outDrops[ki].fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            OutFrame f;
            f.kind = ki;
            f.len  = len;
            memcpy(f.b, frame, len);
            if (xQueueSend(s_out, &f, 0) != pdTRUE) {
                s_outDrops[ki].fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            return true;
        }

        Staged<EmitterCfg>& emitterCfg() { return s_cfg; }

        void requestSendNow(uint8_t idx)
        {
            if (!s_sendNowQ || xQueueSend(s_sendNowQ, &idx, 0) != pdTRUE)
                s_sendNowDrops.fetch_add(1, std::memory_order_relaxed);
        }

        SendNow sendNowWait(uint8_t idx)
        {
            RR::Args a;
            a.op = OP_SEND_NOW;
            a.u[0] = idx;
            RR::Result r;
            if (s_rr.call(a, r, postTicket, nullptr) != RR::Channel::Call::Ok) return SendNow::Busy;
            return r.status ? SendNow::Sent : SendNow::NoData;
        }

        bool preview(uint8_t idx, uint8_t* frame, uint8_t& len)
        {
            if (!s_s.state) return false;
            // A scratch emitter: the hu task's own is never read off its task.
            // Both in Scratch: the caller is httpd (4 kB stack), and the two
            // hold three field maps (~1.2 kB).
            struct Work { EmitterCfg c; Canbox::Rav4Emitter e; };
            Scratch<Work> w;
            if (!w || !s_cfg.peek(w->c)) return false;
            w->e.setTuning(w->c.tuning);
            w->e.setFieldMap(w->c.fields);
            return w->e.preview(idx, *s_s.state, frame, len);
        }

        bool sendByCmd(uint8_t cmd)
        {
            s_replyMode = true;
            const bool ok = s_s.emitter->sendByCmd(cmd, *s_s.state);
            s_replyMode = false;
            return ok;
        }
    }
}
