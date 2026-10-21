#include "LedStrip.h"
#if FEATURE_KNOBS
#include <Arduino.h>
#include <driver/rmt.h>
#include <esp_intr_alloc.h>
#include <soc/soc_caps.h>
#include <string.h>

namespace Knobs
{
    static constexpr rmt_channel_t CH = RMT_CHANNEL_0;

    // 80 MHz APB / 2 = 25 ns per tick. WS2812B wants 0.4/0.85 us for a zero and
    // 0.8/0.45 us for a one, +-150 ns — so these have room on both sides.
    // SK6812 wants 0.3/0.9 and 0.6/0.6: its one-high tops out at 0.75 us, so
    // an RGBW strip gets its own (0.35/0.9, 0.7/0.55, inside both specs); the
    // WS2812B rings keep the timing they were proven on.
    static constexpr uint8_t  CLK_DIV = 2;
    static constexpr uint32_t T0H = 16, T0L = 34, T1H = 32, T1L = 18;
    static constexpr uint32_t S0H = 14, S0L = 36, S1H = 28, S1L = 22;

    // An rmt_item32_t, spelled as its raw word: duration0 | level0 << 15 |
    // duration1 << 16 | level1 << 31. High first, then low.
    static constexpr uint32_t item(uint32_t hi, uint32_t lo) { return hi | (1u << 15) | (lo << 16); }
    // Read by the refill ISR; set in show() only between frames.
    static volatile uint32_t s_zero = item(T0H, T0L);
    static volatile uint32_t s_one  = item(T1H, T1L);

    // Bytes in wire order. Not the caller's buffer: the driver reads it for the
    // whole frame while the caller repaints. Four a LED for RGBW.
    static uint8_t s_wire[MAX_LEDS * 4];

    // Bytes -> RMT items, called by the driver from its ISR as memory drains
    // (480 bytes for 160 LEDs instead of ~15 kB of pre-built items).
    static void IRAM_ATTR toItems(const void* src, rmt_item32_t* dest, size_t srcSize,
                                  size_t wanted, size_t* translated, size_t* itemNum)
    {
        if (!src || !dest) { *translated = 0; *itemNum = 0; return; }
        const uint8_t* p = static_cast<const uint8_t*>(src);
        size_t size = 0, num = 0;
        // Whole bytes only: eight items or none.
        while (size < srcSize && num + 8 <= wanted) {
            const uint8_t v = *p++;
            for (int8_t bit = 7; bit >= 0; --bit, ++dest) dest->val = (v >> bit) & 1 ? s_one : s_zero;
            num += 8;
            ++size;
        }
        *translated = size;
        *itemNum = num;
    }

    bool LedStrip::begin(int8_t pin)
    {
        if (pin < 0) return false;

        rmt_config_t c = {};
        c.rmt_mode      = RMT_MODE_TX;
        c.channel       = CH;
        c.gpio_num      = (gpio_num_t)pin;
        c.clk_div       = CLK_DIV;
        c.tx_config.carrier_en     = false;
        c.tx_config.loop_en        = false;
        c.tx_config.idle_output_en = true;
        c.tx_config.idle_level     = RMT_IDLE_LEVEL_LOW;

        // Every RMT block (nothing else uses RMT): more memory = more ISR
        // latency slack, 320 us on the classic part. Falls back
        // to one block.
        c.mem_block_num = SOC_RMT_CHANNELS_PER_GROUP;
        if (rmt_config(&c) != ESP_OK) {
            c.mem_block_num = 1;
            if (rmt_config(&c) != ESP_OK) return false;
        }
        // IRAM interrupt: the refill ISR (toItems, ~20 refills per 160-LED
        // frame) must keep running while flash is busy (the LittleFS log, NVS
        // saves), or the RMT wraps and clocks out stale items as a garbled
        // frame. toItems is IRAM_ATTR and s_wire is DRAM. Plain interrupt as a
        // fallback rather than no LEDs (uninstall first: a half install would
        // make the second attempt "already installed").
        if (rmt_driver_install(CH, 0, ESP_INTR_FLAG_IRAM) != ESP_OK) {
            rmt_driver_uninstall(CH);
            if (rmt_driver_install(CH, 0, 0) != ESP_OK) return false;
        }
        if (rmt_translator_init(CH, toItems) != ESP_OK) return false;

        _ok = true;
        return true;
    }

    // Does not wait: a 160-LED frame is 4.8 ms on the wire, and the caller samples
    // the encoder every millisecond.
    bool LedStrip::show(const uint8_t* rgb, uint8_t count, Order order)
    {
        if (!_ok || !rgb || !count) return false;
        if (count > MAX_LEDS) count = MAX_LEDS;

        // Still sending the last one: skip this frame, the next is 20 ms away.
        if (rmt_wait_tx_done(CH, 0) != ESP_OK) { ++_busy; return false; }

        const uint16_t n = packWire(rgb, count, order, s_wire);
        const bool sk = order == Order::GRBW;
        s_zero = sk ? item(S0H, S0L) : item(T0H, T0L);
        s_one  = sk ? item(S1H, S1L) : item(T1H, T1L);

        // Idles low after; the >= 15 ms gap far exceeds the 50 us (WS2812B) /
        // 80 us (SK6812) latch time.
        if (rmt_write_sample(CH, s_wire, n, false) != ESP_OK) return false;
        ++_frames;
        return true;
    }
}
#endif
