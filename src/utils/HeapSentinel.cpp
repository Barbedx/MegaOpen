#include "HeapSentinel.h"
#if HEAP_SENTINEL
#include <Arduino.h>
#include <atomic>
#include <string.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <esp_rom_uart.h>
#include <esp_system.h>
#include "LogBus.h"

namespace HeapSentinel
{
    namespace
    {
        constexpr uint32_t RING = 512;
        DRAM_ATTR char              s_ring[RING];
        DRAM_ATTR volatile uint32_t s_w = 0;
        std::atomic<uint32_t> s_checks{0};
        std::atomic<uint32_t> s_maxUs{0};
        std::atomic<bool>     s_fired{false};

        // The ROM's printf hands over one character at a time, from any
        // context (the panic handler too): IRAM, no lock, nothing off-ROM
        // called. A character lost to a race is nothing against a lock here.
        void IRAM_ATTR putcHook(char c)
        {
            const uint32_t w = s_w;
            s_ring[w % RING] = c;
            s_w = w + 1;
            esp_rom_uart_putc(c);   // this libs package has no esp_rom_output_putc yet
        }

        // What the ROM wrote since `from`, newlines as '|', into out.
        void since(uint32_t from, char* out, size_t n)
        {
            const uint32_t w = s_w;
            if (w - from > RING) from = w - RING;
            size_t k = 0;
            for (uint32_t i = from; i < w && k + 1 < n; ++i) {
                const char c = s_ring[i % RING];
                if (c == '\r') continue;
                out[k++] = c == '\n' ? '|' : c;
            }
            out[k] = 0;
        }

        // "CORRUPT HEAP: Bad head at 0x3fcdb820. Expected ..." -> 0x3fcdb820, or 0.
        uintptr_t addrIn(const char* s)
        {
            const char* p = strstr(s, " at 0x");
            return p ? (uintptr_t)strtoul(p + 4, nullptr, 16) : 0;
        }

        // ESP32-S3 internal SRAM on the data bus.
        bool inDram(uintptr_t a) { return a >= 0x3FC88000u && a < 0x3FD00000u; }

        // 128 bytes around the bad canary, 32 a line, the canary's own row third.
        void hexDump(uintptr_t at)
        {
            char b[3 * 32 + 8 + 1];
            for (int row = -2; row < 2; ++row) {
                const uintptr_t a = at + (intptr_t)row * 32;
                if (!inDram(a) || !inDram(a + 31)) continue;
                int n = 0;
                for (int i = 0; i < 32; ++i)
                    n += snprintf(b + n, sizeof(b) - n, "%02x%s", ((const uint8_t*)a)[i], (i & 3) == 3 ? " " : "");
                LOGW("heap", "%08x: %s", (unsigned)a, b);
            }
        }
    }

    void begin()
    {
        esp_rom_install_channel_putc(1, putcHook);
    }

    bool check(const char* where)
    {
        const uint32_t from = s_w;
        const uint32_t t0 = micros();
        const bool ok = heap_caps_check_integrity(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, true);
        const uint32_t us = micros() - t0;
        uint32_t m = s_maxUs.load();
        while (us > m && !s_maxUs.compare_exchange_weak(m, us)) {}
        ++s_checks;
        if (ok) return true;
        bool was = false;
        if (!s_fired.compare_exchange_strong(was, true)) return false;   // the first finder reports
        char text[160];
        since(from, text, sizeof(text));
        const char* task = pcTaskGetName(nullptr);
        LOGW("heap", "CORRUPT at %s on task %s (walk %u, up %u ms): %s", where, task ? task : "?",
             (unsigned)s_checks.load(), (unsigned)millis(), text);
        const uintptr_t a = addrIn(text);
        if (a) hexDump(a);
        else   LOGW("heap", "no address in the checker's text: the ROM putc hook saw nothing");
        LogBus::flushAndWait(800);   // the flash log, before the coredump
        char msg[128];
        snprintf(msg, sizeof(msg), "HEAP corrupt at %s on %s: %.72s", where, task ? task : "?", text);
        esp_system_abort(msg);
        return false;
    }

    uint32_t checks() { return s_checks.load(); }
    uint32_t maxUs()  { return s_maxUs.load(); }
}
#endif
