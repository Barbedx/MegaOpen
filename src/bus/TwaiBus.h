#pragma once
#include <Arduino.h>
#include <atomic>
#include <driver/twai.h>
#include "ICanBus.h"
#include "Frame.h"

// The CAN bus, straight on the IDF TWAI driver: install, start, poll, transmit.
//   TWAI_GENERAL_CONFIG_DEFAULT(tx, rx, mode)   // TX first in this macro
//   rx_queue_len 64: the default 5 overruns silently at ~1000 frames/s.
//   Accept-all filter, then drain with twai_receive(&m, 0).
class TwaiBus : public ICanBus {
public:
    static TwaiBus& instance();

    // mode: TWAI_MODE_NORMAL (we ACK and transmit) or TWAI_MODE_LISTEN_ONLY
    // (never transmits, never ACKs, cannot be driven bus-off).
    bool begin(gpio_num_t txPin, gpio_num_t rxPin, uint32_t bitrate, twai_mode_t mode);
    void end();
    bool restart();                       // stop + start, same config

    // Drain everything to the onReceive() handler. Re-entry guarded: it is called
    // from inside the display's ACK wait, and an RX handler may send.
    void poll();

    // One frame, non-blocking, straight off the driver queue. AffaDisplay's
    // ICanLink is a pull port (a push seam made an ACK deadlock possible), so the
    // link uses this instead of poll(). A frame waitRx() stashed comes first.
    bool recvOne(Frame& out);

    // The owner task's sleep: block in twai_receive() for up to `ticks`, and
    // keep the frame that ends it in a one-frame stash for the next recvOne()
    // or poll(). That is what lets the task that owns the bus sleep between
    // frames and still wake on a panel key the moment it arrives, instead of a
    // fixed delay(2). With no driver installed it is a plain vTaskDelay.
    // The stash is dropped with the driver (end()): a frame from before a
    // reinstall belongs to a different bus configuration.
    void waitRx(TickType_t ticks);
    bool stashed() const { return _stashed; }

    // Bus-off / stopped recovery only, no draining. The display role never calls
    // poll(), so without this the first bus-off is permanent.
    void maintain();

    // Drop every frame waiting to transmit, and the one the controller is
    // sending right now. The panel task's TX gate: with no one ACKing, the
    // controller would otherwise retransmit that frame for ever. Owner task
    // only; no reinstall, so installs() does not move. False = the controller
    // was not running (bus-off, recovering) and only the queue was cleared.
    bool abortTx();

    using TxTap = void (*)(const Frame&);
    void onTransmit(TxTap t) { _txTap = t; }   // observe what we put on the wire

    // Busy = the driver's 8-slot TX queue is momentarily full (a 96-byte screen is
    // 13 frames); the frame never reached the bus, so re-offering it is free.
    // AffaDisplay's trySend() FSM retries Busy without spending its retry budget.
    // Failed = not installed, gate shut, listen-only or driver refusal.
    enum class TxResult : uint8_t { Ok, Busy, Failed };
    TxResult trySend(const Frame& f);

    // Non-blocking: it runs on the task that owns AffaDisplay's deadlines, and
    // the library's contract says send() must not block.
    bool send(const Frame& f) override;
    void onReceive(RxHandler h, void* ctx) override { _rx = h; _rxCtx = ctx; }
    bool isLive() const override { return _installed.load(std::memory_order_relaxed); }
    void setTxEnabled(bool on) { _txEnabled = on; }
    bool txEnabled() const { return _txEnabled; }

    bool running() const;
    twai_mode_t mode() const { return _mode; }
    uint32_t bitrate() const { return _bitrate; }
    uint32_t rxCount() const { return _rxCount; }
    uint32_t txOk() const { return _txOk; }
    uint32_t txFail() const { return _txFail; }
    // Not a failure: climbing txBusy with txFail 0 means the queue, not the bus.
    uint32_t txBusy() const { return _txBusy; }
    // Driver installs since boot; every one after the first is a reinstall
    // (the loopback test, a STOPPED controller restarted).
    uint32_t installs() const { return _installs; }

private:
    TwaiBus() = default;
    bool install();

    RxHandler   _rx = nullptr;
    void*       _rxCtx = nullptr;
    gpio_num_t  _tx = GPIO_NUM_NC, _rxPin = GPIO_NUM_NC;
    uint32_t    _bitrate = 500000;
    twai_mode_t _mode = TWAI_MODE_NORMAL;
    // Atomic: one task installs and removes the driver (the loop, which owns
    // the bus), but httpd and the cloud read isLive() for their pages.
    std::atomic<bool> _installed{false};
    volatile bool _txEnabled = true;
    bool        _inPoll = false;   // re-entry guard (see poll())
    uint32_t    _rxCount = 0, _txOk = 0, _txFail = 0, _txBusy = 0;
    uint32_t    _installs = 0;
    // waitRx()'s frame, owner task only.
    Frame       _stash;
    bool        _stashed = false;

    TxTap       _txTap = nullptr;
};
