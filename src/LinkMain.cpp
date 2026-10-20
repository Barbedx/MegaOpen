// BOARD V — the ESP32-C3 on the car's OBD2 CAN. See docs/link.md and
// src/link/LinkProto.h.
//
// Listens to the car bus and hands frames up a UART to the brain. No web UI,
// no NVS, no settings: the brain pushes a filter over the same wire.
// Listen-only at the controller, so it cannot ACK or transmit, and cannot be
// told to over the link.
#ifndef MEGAOPEN_BOARDV
#define MEGAOPEN_BOARDV 0
#endif

#if MEGAOPEN_BOARDV

#include <Arduino.h>
#include <driver/twai.h>
#include "board/BoardProfile.h"
#include "bus/TwaiBus.h"
#include "link/LinkProto.h"

namespace {

// The link uses the C3 profile's head-unit pins (no head unit here).
// GPIO21 = our TX (to the brain's RX), GPIO20 = our RX.
constexpr int8_t kLinkTx = Board::PIN_HU_TX;
constexpr int8_t kLinkRx = Board::PIN_HU_RX;
// 2: Status carries the simulator rate, so its record grew from 17 to 19 bytes.
constexpr uint8_t kVersion = 2;

Link::ChangeFilter g_filter;
Link::Decoder      g_down;              // what the brain sends us
uint32_t g_frames = 0, g_forwarded = 0, g_dropped = 0;
uint32_t g_fpsCount = 0, g_fps = 0, g_fpsAt = 0;
uint32_t g_statusAt = 0;

void onBus(const Frame& f, void*);      // the door simulated frames come in through

// ---- the emulator ------------------------------------------------------------
// Simulated frames enter through onBus(), exercising everything but the
// transceiver. Megane ids (docs/megane-can-ids.md); two payloads change every
// frame and two never do, so F_CHANGES should forward only the first two.
uint16_t g_simFps = 0;
uint32_t g_simAt = 0, g_simAcc = 0, g_simSeq = 0;

void simTick(uint32_t now)
{
    if (!g_simFps) { g_simAt = now; return; }
    const uint32_t dtMs = now - g_simAt;
    if (!dtMs) return;
    g_simAt = now;
    // Milli-frame accumulator, so uneven rates still average out.
    g_simAcc += dtMs * g_simFps;
    uint32_t due = g_simAcc / 1000;
    if (!due) return;
    g_simAcc -= due * 1000;
    if (due > 64) due = 64;             // never outrun the UART in one pass
    static const uint32_t kIds[4] = { 0x181, 0x354, 0x5DE, 0x60D };
    while (due--) {
        Frame f{};
        const uint32_t n = g_simSeq++;
        f.id  = kIds[n & 3];
        f.len = 8;
        f.source = Frame::SRC_VH_CAN;
        if ((n & 3) < 2) {              // two that move: a ramp and a counter
            f.data[0] = (uint8_t)(n >> 2);
            f.data[1] = (uint8_t)((n >> 10) & 0xFF);
            f.data[2] = (uint8_t)(n & 3);
        } else {                        // two that never change
            f.data[0] = 0xA5; f.data[1] = (uint8_t)(n & 3);
        }
        f.data[7] = 0x5A;               // a byte that says "made up" in a hex dump
        onBus(f, nullptr);
    }
}

// Returns whether the record actually went out.
bool send(const uint8_t* rec, uint8_t n)
{
    // Never block the CAN drain on the UART. 128 bytes of TX FIFO plus the
    // Arduino ring: if the link is behind, the frame is dropped and counted.
    if (Serial1.availableForWrite() < n) { ++g_dropped; return false; }
    Serial1.write(rec, n);
    return true;
}

void onBus(const Frame& f, void*)
{
    ++g_frames;
    ++g_fpsCount;
    if (!g_filter.pass(f, millis())) return;
    uint8_t rec[Link::MAX_RECORD];
    if (send(rec, Link::encodeCan(f, rec))) ++g_forwarded;
}

void sendStatus()
{
    twai_status_info_t st{};
    Link::Status s{};
    s.frames    = g_frames;
    s.forwarded = g_forwarded;
    s.fps       = (uint16_t)(g_fps > 65535 ? 65535 : g_fps);
    s.mode      = (uint8_t)g_filter.mode();
    s.sim       = g_simFps;
    if (twai_get_status_info(&st) == ESP_OK) {
        s.busErr = st.bus_error_count;
        s.rxErr = (uint8_t)(st.rx_error_counter > 255 ? 255 : st.rx_error_counter);
        s.state = (uint8_t)st.state;
    }
    uint8_t rec[Link::MAX_RECORD];
    send(rec, Link::encodeStatus(s, rec));
}

// The simulator switch from the USB console, for when the link itself is in
// question: a number + Enter sets frames/s, `0` stops.
void drainConsole()
{
    static char buf[12];
    static uint8_t n = 0;
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            if (!n) continue;
            buf[n] = 0;
            n = 0;
            const long v = atol(buf);
            if (v < 0 || v > 65535) { Serial.println("[boardV] rate must be 0..65535"); continue; }
            g_simFps = (uint16_t)v;
            g_simAcc = 0;
            g_simAt  = millis();
            Serial.printf("[boardV] SIM %s (%u frames/s)\n", v ? "on" : "off", (unsigned)v);
            sendStatus();
        } else if (n < sizeof(buf) - 1 && c >= ' ') {
            buf[n++] = c;
        }
    }
}

void drainDownlink()
{
    Link::Record r;
    while (Serial1.available()) {
        if (!g_down.feed((uint8_t)Serial1.read(), r)) continue;
        Link::FilterMode mode;
        uint32_t ids[Link::MAX_FILTER_IDS];
        uint8_t n;
        if (r.asFilter(mode, ids, n)) {
            g_filter.setMode(mode);
            g_filter.setList(ids, n);
            g_filter.reset();
            Serial.printf("[link] filter mode=%u ids=%u\n", (unsigned)mode, (unsigned)n);
            sendStatus();
        } else if (uint16_t fps; r.asSim(fps)) {
            g_simFps = fps;
            g_simAcc = 0;
            g_simAt  = millis();
            Serial.printf("[link] SIM %s (%u frames/s)\n", fps ? "on" : "off", (unsigned)fps);
            sendStatus();
        } else if (r.type == Link::T_HELLO) {
            uint8_t rec[Link::MAX_RECORD];
            send(rec, Link::encodeHello(kVersion, rec));
            sendStatus();
        }
    }
}

}  // namespace

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial1.begin(Link::BAUD, SERIAL_8N1, kLinkRx, kLinkTx);

    // Listen-only, not a setting. C3 profile pins:
    //   GPIO3 <-- CRX, GPIO4 --> CTX — with TX never enabled, CTX is never driven.
    const bool ok = TwaiBus::instance().begin((gpio_num_t)Board::PIN_CAN_TX,
                                              (gpio_num_t)Board::PIN_CAN_RX,
                                              500000, TWAI_MODE_LISTEN_ONLY);
    TwaiBus::instance().setTxEnabled(false);
    TwaiBus::instance().onReceive(onBus, nullptr);

    uint8_t rec[Link::MAX_RECORD];
    send(rec, Link::encodeHello(kVersion, rec));
    Serial.printf("[boardV] can=%s rx=GPIO%d tx=GPIO%d listen-only  link=GPIO%d->brain GPIO%d<-brain @%lu\n",
                  ok ? "up" : "FAILED", (int)Board::PIN_CAN_RX, (int)Board::PIN_CAN_TX,
                  (int)kLinkTx, (int)kLinkRx, (unsigned long)Link::BAUD);
}

void loop()
{
    // poll() drains the RX queue into onBus and recovers bus-off. Nothing else
    // may take long enough for 64 frames to pile up (40 ms at 1500 frames/s).
    TwaiBus::instance().poll();
    drainDownlink();
    drainConsole();

    const uint32_t now = millis();
    simTick(now);
    if ((uint32_t)(now - g_fpsAt) >= 1000) {
        g_fpsAt = now;
        g_fps = g_fpsCount;
        g_fpsCount = 0;
    }
    if ((uint32_t)(now - g_statusAt) >= 1000) {
        g_statusAt = now;
        sendStatus();
        static uint32_t s_log = 0;
        if ((uint32_t)(now - s_log) >= 5000) {
            s_log = now;
            Serial.printf("[boardV] frames=%lu fwd=%lu fps=%lu dropped=%lu tracked=%u\n",
                          (unsigned long)g_frames, (unsigned long)g_forwarded, (unsigned long)g_fps,
                          (unsigned long)g_dropped, (unsigned)g_filter.tracked());
        }
    }
    delay(1);
}

#endif  // MEGAOPEN_BOARDV
