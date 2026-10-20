#include "Mcp2518Source.h"
#if FEATURE_CAN2
#include <SPI.h>
#include <ACAN2517FD.h>
#include "../board/BoardProfile.h"
#include "../utils/LogBus.h"

// Its own SPI host: the default SPI object belongs to nobody here today, but the
// controller should not be the reason that changes.
static SPIClass   g_spi(HSPI);
static ACAN2517FD g_can(Board::PIN_CAN2_CS, g_spi, Board::PIN_CAN2_INT);

// The ISR's two words: a flag for the car task's next look, and the task to
// wake. See the class comment for why it is ours and not the library's.
static volatile bool         s_intPending = false;
static TaskHandle_t volatile s_notify = nullptr;

static void IRAM_ATTR canIsr()
{
    s_intPending = true;
    TaskHandle_t t = s_notify;
    if (t) {
        BaseType_t woke = pdFALSE;
        vTaskNotifyGiveFromISR(t, &woke);
        if (woke) portYIELD_FROM_ISR();
    }
}

namespace
{
    constexpr uint32_t NO_REQUEST = 0xFFFFFFFFu;

    // What says "wrong rate" while listening (MCP2518FD datasheet DS20006027,
    // CiBDIAG0/1; NOT YET CHECKED ON THE CHIP). The controller's TEC/REC are
    // documented as reset and inactive in listen-only, so they are read but
    // not relied on. Receive-side only: bit and ACK errors are a transmitter's,
    // and we never transmit.
    //   BDIAG1: DCRCERR 29, DSTUFERR 28, DFORMERR 27, NCRCERR 21, NSTUFERR 20,
    //           NFORMERR 19 (sticky flags; the reset in end() clears them)
    //   BDIAG0: DRERRCNT 23:16, NRERRCNT 7:0 (receive error counts)
    constexpr uint32_t BDIAG1_RX_ERRORS = (1u << 29) | (1u << 28) | (1u << 27) |
                                          (1u << 21) | (1u << 20) | (1u << 19);
    constexpr uint32_t BDIAG0_RX_COUNTS = 0x00FF00FFu;

    // How often the search reads the error registers (SPI, ~20 us each).
    constexpr uint32_t ERR_CHECK_MS   = 100;
    // A begin() that failed for any reason but a mode timeout: the chip itself.
    constexpr uint32_t RETRY_MS       = 5000;
    // "Still no bus": once after the first fruitless round, then this often.
    constexpr uint32_t QUIET_LOG_MS   = 600000;
    constexpr uint32_t PUBLISH_MS     = 250;
    // The locked controller's error registers, for the status only.
    constexpr uint32_t STATUS_READ_MS = 1000;

    inline unsigned kbps(uint32_t bps) { return (unsigned)(bps / 1000); }
}

Mcp2518Source& Mcp2518Source::instance()
{
    static Mcp2518Source s;
    return s;
}

void Mcp2518Source::configure(uint32_t manualBps, uint32_t savedBps)
{
    _manual = Bus::Autobaud::valid(manualBps) ? manualBps : 0;
    _saved  = Bus::Autobaud::valid(savedBps) ? savedBps : 0;
}

void Mcp2518Source::setNotifyTask(TaskHandle_t t) { s_notify = t; }

void Mcp2518Source::requestRate(uint32_t manualBps)
{
    _req.store(manualBps, std::memory_order_release);
}

uint32_t Mcp2518Source::bitrate() const
{
    return (_live && _ab.state() != Bus::Autobaud::State::Searching) ? _ab.rate() : 0;
}

void Mcp2518Source::stop()
{
    // end() detaches the ISR, resets the chip, deletes the library's handler
    // task and frees its rings and callback array: nothing of this rate is
    // left behind. It masks interrupts on this core for up to ~2 ms while it
    // waits for configuration mode.
    if (_begun) g_can.end();
    _begun = false;
    _live = false;
}

bool Mcp2518Source::start(uint32_t bps, uint32_t nowMs)
{
    stop();
    ACAN2517FDSettings s(ACAN2517FDSettings::OSC_40MHz, bps, DataBitRateFactor::x1);
    s.mRequestedMode = ACAN2517FDSettings::ListenOnly;
    // The car bus runs ~1500 frames/s; 256 is ~170 ms of slack against a
    // stalled car task, and the chip's own FIFO is behind that.
    s.mDriverReceiveFIFOSize = 256;
    // Nothing is ever sent: give the chip's RAM to reception.
    s.mDriverTransmitFIFOSize = 1;
    s.mControllerTransmitFIFOSize = 1;
    s.mControllerTXQSize = 0;
    // 8-byte slots: classic frames only, and 32 of them fit the chip's 2 KB RAM
    // where 32 of the default 64-byte slots would make begin() fail.
    s.mControllerTransmitFIFOPayload = ACAN2517FDSettings::PAYLOAD_8;
    s.mControllerReceiveFIFOPayload = ACAN2517FDSettings::PAYLOAD_8;
    s.mControllerReceiveFIFOSize = 32;

    const uint32_t err = g_can.begin(s, canIsr);
    _begun = true;           // even a failed begin() may have attached and created
    ++_restarts;
    _beginErr = err;
    _rateFrames = 0;
    _ab.started(nowMs);
    _errCheckAtMs = nowMs;
    if (err) {
        // A half-started controller (mode timeout) would keep its ISR attached
        // and its task alive with nobody draining: stop it.
        stop();
        return false;
    }
    _live = true;
    // INT is armed on a FALLING edge only after the chip has gone listen-only.
    // A frame in between leaves INT low with no edge to come, so look now
    //; drain() keeps looking at the level.
    s_intPending = true;
    return true;
}

bool Mcp2518Source::begin()
{
    if (!_spiUp) {
        g_spi.begin(Board::PIN_CAN2_SCK, Board::PIN_CAN2_MISO, Board::PIN_CAN2_MOSI);
        _spiUp = true;
    }
    const uint32_t now = millis();
    _ab.begin(_manual, _saved, now);
    const bool ok = start(_ab.rate(), now);
    if (_manual)
        LogBus::printf("[can2] MCP2518FD listen-only at %uk, set by hand; cs=GPIO%d int=GPIO%d%s",
                       kbps(_manual), (int)Board::PIN_CAN2_CS, (int)Board::PIN_CAN2_INT,
                       ok ? "" : " - begin FAILED");
    else
        LogBus::printf("[can2] MCP2518FD listen-only, finding the rate: %uk first (%s); "
                       "cs=GPIO%d int=GPIO%d%s",
                       kbps(_ab.rate()), _saved ? "saved" : "default",
                       (int)Board::PIN_CAN2_CS, (int)Board::PIN_CAN2_INT,
                       ok ? "" : " - begin FAILED");
    if (!ok) {
        LogBus::printf("[can2] begin err=0x%lX (no chip, or a 2515 board?); retrying every %u s",
                       (unsigned long)_beginErr, (unsigned)(RETRY_MS / 1000));
        _retryAtMs = now + RETRY_MS;
    }
    publish(now);
    return ok;
}

void Mcp2518Source::service(uint32_t nowMs)
{
    // A rate set (or cleared) on the web: start the search over.
    const uint32_t req = _req.exchange(NO_REQUEST, std::memory_order_acq_rel);
    if (req != NO_REQUEST) {
        _manual = Bus::Autobaud::valid(req) ? req : 0;
        _ab.begin(_manual, _saved, nowMs);
        const bool ok = start(_ab.rate(), nowMs);
        LogBus::printf("[can2] rate %s: listening at %uk%s",
                       _manual ? "set by hand" : "back to auto", kbps(_ab.rate()),
                       ok ? "" : " - begin FAILED");
        if (!ok) _retryAtMs = nowMs + RETRY_MS;
    }

    if (!_live) {
        if (_spiUp && (int32_t)(nowMs - _retryAtMs) >= 0) {
            const bool ok = start(_ab.rate(), nowMs);
            if (!ok) {
                const bool modeOnly = _beginErr == ACAN2517FD::kRequestedModeTimeOut;
                // Listen-only is reached at the first bus idle; a wrong rate
                // on a busy bus can miss that within begin()'s 10 ms. That is a
                // verdict on the rate, not the chip: move on. Not at once: a
                // bus held dominant never goes idle at any rate, and begin()
                // spins 10 ms each time, so the next try waits one dwell.
                if (modeOnly && _ab.state() == Bus::Autobaud::State::Searching) {
                    _ab.observe(nowMs, 0, 1);
                    _retryAtMs = nowMs + Bus::Autobaud::DEFAULT_DWELL_MS;
                } else {
                    _retryAtMs = nowMs + RETRY_MS;
                }
            }
        }
    } else if (_ab.state() == Bus::Autobaud::State::Searching &&
               (uint32_t)(nowMs - _errCheckAtMs) >= ERR_CHECK_MS) {
        _errCheckAtMs = nowMs;
        const uint32_t d0 = g_can.diagInfos(0);
        _bdiag1 = g_can.diagInfos(1);
        _trec = g_can.errorCounters();
        const uint32_t errors = ((_bdiag1 & BDIAG1_RX_ERRORS) ? 1u : 0u) +
                                ((d0 & BDIAG0_RX_COUNTS) ? 1u : 0u) + (_trec & 0xFFu);
        const uint32_t was = _ab.rate();
        const uint32_t cyclesWas = _ab.cycles();
        switch (_ab.observe(nowMs, _rateFrames, errors)) {
            case Bus::Autobaud::Step::Lock:
                LogBus::printf("[can2] bus found: %uk, %lu clean frames, listen-only",
                               kbps(was), (unsigned long)_rateFrames);
                break;
            case Bus::Autobaud::Step::Next:
                // Only the first round is told rate by rate; after that the
                // search goes round quietly and says so every 10 minutes.
                if (errors && cyclesWas == 0)
                    LogBus::printf("[can2] %uk: errors (bdiag1=0x%08lX) after %lu frames - "
                                   "not this rate", kbps(was), (unsigned long)_bdiag1,
                                   (unsigned long)_rateFrames);
                if (_ab.cycles() != cyclesWas &&
                    (_ab.cycles() == 1 || (uint32_t)(nowMs - _quietLogAtMs) >= QUIET_LOG_MS)) {
                    _quietLogAtMs = nowMs;
                    LogBus::printf("[can2] no clean frames at 500k/250k/125k/1M (round %lu, "
                                   "%lu rejected for errors); still listening",
                                   (unsigned long)_ab.cycles(), (unsigned long)_ab.rejected());
                }
                if (!start(_ab.rate(), nowMs)) _retryAtMs = nowMs;
                break;
            case Bus::Autobaud::Step::Stay:
                break;
        }
    }

    if ((uint32_t)(nowMs - _fpsAtMs) >= 1000) {
        _fpsAtMs = nowMs;
        _fps = _fpsCount;
        _fpsCount = 0;
    }
    if ((uint32_t)(nowMs - _publishAtMs) >= PUBLISH_MS) publish(nowMs);
}

void Mcp2518Source::publish(uint32_t nowMs)
{
    static uint32_t s_readAtMs = 0;
    // The status registers, once a second, on this task (it owns the SPI).
    // While searching, service() has just read them.
    if (_live && _ab.state() != Bus::Autobaud::State::Searching &&
        (uint32_t)(nowMs - s_readAtMs) >= STATUS_READ_MS) {
        s_readAtMs = nowMs;
        _trec = g_can.errorCounters();
        _bdiag1 = g_can.diagInfos(1);
    }
    _publishAtMs = nowMs;
    Status s;
    s.live          = _live;
    s.state         = (uint8_t)_ab.state();
    s.bps           = _ab.rate();
    s.detected      = _ab.detected();
    s.fps           = _fps;
    s.frames        = _frames;
    s.fdDropped     = _fdDropped;
    s.remoteDropped = _remoteDropped;
    s.intKicks      = _intKicks;
    s.maskMaxUs     = _maskMaxUs;
    if (_live) {
        s.hwOverflows = g_can.hardwareReceiveBufferOverflowCount();
        s.ringPeak    = g_can.driverReceiveBufferPeakCount();
    }
    s.trec          = _trec;
    s.bdiag1        = _bdiag1;
    s.restarts      = _restarts;
    s.cycles        = _ab.cycles();
    s.rejected      = _ab.rejected();
    s.beginErr      = _beginErr;
    _snap.publish(s);
}

size_t Mcp2518Source::drain(Frame* out, size_t max, uint32_t)
{
    if (!_live) return 0;
    // Service the chip on a flag, or on INT simply being low: the level is the
    // truth, an edge can be missed.
    if (s_intPending || digitalRead(Board::PIN_CAN2_INT) == LOW) {
        if (!s_intPending) ++_intKicks;
        s_intPending = false;
        const uint32_t t0 = micros();
        // Masks interrupts on this core while it drains the chip's FIFO into
        // the driver ring (ACAN2517FD.cpp:891-933).
        g_can.isr_poll_core();
        const uint32_t dt = micros() - t0;
        if (dt > _maskMaxUs) _maskMaxUs = dt;
    }
    size_t n = 0;
    CANFDMessage m;
    // receive() only touches SPI to re-enable the RX interrupt after the ring
    // was full; the ring itself is this task's alone now.
    while (n < max && g_can.receive(m)) {
        if (m.type != CANFDMessage::CAN_DATA) {
            // FD frames do not fit Frame; a remote frame has no payload and
            // would be passed on as stale bytes.
            if (m.type == CANFDMessage::CAN_REMOTE) ++_remoteDropped;
            else                                    ++_fdDropped;
            continue;
        }
        Frame& f = out[n++];
        f.id = m.id;
        f.extended = m.ext;
        f.len = m.len > 8 ? 8 : m.len;
        for (uint8_t i = 0; i < 8; ++i) f.data[i] = i < f.len ? m.data[i] : 0;
        f.source = Frame::SRC_VH_CAN;
    }
    _frames += (uint32_t)n;
    _rateFrames += (uint32_t)n;
    _fpsCount += (uint32_t)n;
    return n;
}

IVehicleSource::Stats Mcp2518Source::stats() const
{
    Stats s;
    s.frames  = _frames;
    s.dropped = _fdDropped + _remoteDropped;
    return s;
}
#endif
