#pragma once
#include "../Features.h"
#if FEATURE_VEHICLE_CAN && !FEATURE_DISPLAY_CAN
#include <Arduino.h>
#include <atomic>
#include <driver/twai.h>
#include "Frame.h"

// The car's bus on the TWAI, in a build with no panel (env:direct): a listen-
// only sniffer, straight on the IDF driver. Where there is a panel the TWAI is
// the panel's, and its driver is AffaDisplay's affa::TwaiLink.
//
// Not TwaiLink here: the library cannot be linked into a build with no panel.
// AffaConfig.h stops a build that names no panel family, so linking it would
// compile a panel's protocol into the one image that has none.
//
//   TWAI_GENERAL_CONFIG_DEFAULT(tx, rx, mode)   // TX first in this macro
//   rx_queue_len 64: the default 5 overruns silently at ~1000 frames/s.
//   Accept-all filter, then drain with twai_receive(&m, 0).
class TwaiBus {
public:
    // Plain function pointer, not std::function: no heap/RTTI on the ESP32.
    using RxHandler = void (*)(const Frame&, void* ctx);

    static TwaiBus& instance();

    // mode: TWAI_MODE_LISTEN_ONLY (never transmits, never ACKs, cannot be
    // driven bus-off), the only mode this build uses.
    bool begin(gpio_num_t txPin, gpio_num_t rxPin, uint32_t bitrate, twai_mode_t mode);
    void end();
    bool restart();                       // stop + start, same config

    // Drain everything to the onReceive() handler, after bus-off / stopped
    // recovery. Re-entry guarded.
    void poll();

    // The owner task's sleep: block in twai_receive() for up to `ticks`, and
    // keep the frame that ends it in a one-frame stash for the next poll().
    // With no driver installed it is a plain vTaskDelay. The stash is dropped
    // with the driver (end()): a frame from before a reinstall belongs to a
    // different bus configuration.
    void waitRx(TickType_t ticks);

    void onReceive(RxHandler h, void* ctx) { _rx = h; _rxCtx = ctx; }
    bool isLive() const { return _installed.load(std::memory_order_relaxed); }

    bool running() const;
    twai_mode_t mode() const { return _mode; }
    uint32_t bitrate() const { return _bitrate; }
    uint32_t rxCount() const { return _rxCount; }
    // Driver installs since boot; every one after the first is a reinstall
    // (a STOPPED controller restarted).
    uint32_t installs() const { return _installs; }

private:
    TwaiBus() = default;
    bool install();
    void maintain();

    RxHandler   _rx = nullptr;
    void*       _rxCtx = nullptr;
    gpio_num_t  _tx = GPIO_NUM_NC, _rxPin = GPIO_NUM_NC;
    uint32_t    _bitrate = 500000;
    twai_mode_t _mode = TWAI_MODE_LISTEN_ONLY;
    // Atomic: one task installs and removes the driver (the loop, which owns
    // the bus), but httpd reads isLive() for its pages.
    std::atomic<bool> _installed{false};
    bool        _inPoll = false;   // re-entry guard (see poll())
    uint32_t    _rxCount = 0;
    uint32_t    _installs = 0;
    // waitRx()'s frame, owner task only.
    Frame       _stash;
    bool        _stashed = false;
};
#endif
