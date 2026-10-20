#include "TwaiBus.h"
#include <soc/io_mux_reg.h>

// Install lines go through the LogBus, which never blocks: install() runs on
// the task that owns the bus (the loop; the web's /api/can/loop is a
// Diag::CanBench op run there), and a Serial write would wait on the
// console — up to its whole TX timeout on a USB-CDC nobody reads.
#include "../utils/LogBus.h"
#define TWAI_LOGF(...) LogBus::printf(__VA_ARGS__)

TwaiBus& TwaiBus::instance()
{
    static TwaiBus b;
    return b;
}

bool TwaiBus::install()
{
    twai_timing_config_t t;
    switch (_bitrate) {
        case 1000000: t = TWAI_TIMING_CONFIG_1MBITS();   break;
        case 800000:  t = TWAI_TIMING_CONFIG_800KBITS(); break;
        case 500000:  t = TWAI_TIMING_CONFIG_500KBITS(); break;
        case 250000:  t = TWAI_TIMING_CONFIG_250KBITS(); break;
        case 125000:  t = TWAI_TIMING_CONFIG_125KBITS(); break;
        case 100000:  t = TWAI_TIMING_CONFIG_100KBITS(); break;
        case 50000:   t = TWAI_TIMING_CONFIG_50KBITS();  break;
        default:      t = TWAI_TIMING_CONFIG_500KBITS(); break;
    }

    // Macro argument order is (tx, rx, mode), unlike setCANPins(rx, tx).
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(_tx, _rxPin, _mode);
    g.rx_queue_len = 64;   // default 5 is overrun in microseconds at ~1000 fps
    g.tx_queue_len = 8;
    g.alerts_enabled = TWAI_ALERT_BUS_ERROR | TWAI_ALERT_ERR_PASS |
                       TWAI_ALERT_BUS_OFF | TWAI_ALERT_ARB_LOST |
                       TWAI_ALERT_TX_FAILED | TWAI_ALERT_RX_QUEUE_FULL;

    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    // TX pad level before install, after install, after start. Idle TX must be
    // recessive (1); a 0 means the controller clamps the bus.
    // Once the GPIO matrix owns the pad its input buffer is off and digitalRead()
    // returns 0 regardless, so enable the input path before reading.
    auto padRead = [this]() -> int {
        if (_tx < 0 || _tx > 31) return -1;      // the mux table stops there
        PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[_tx]);
        return (REG_READ(GPIO_IN_REG) >> _tx) & 1;
    };
    const int txBefore = digitalRead(_tx);
    const esp_err_t inst = twai_driver_install(&g, &t, &f);
    const int txAfterInstall = (inst == ESP_OK) ? padRead() : -1;
    if (inst != ESP_OK) {
        TWAI_LOGF("[twai] install FAILED (%d)  TX before=%d\n", (int)inst, txBefore);
        return false;
    }
    const esp_err_t st = twai_start();
    const int txAfterStart = (st == ESP_OK) ? padRead() : -1;
    TWAI_LOGF("[twai] mode=%d TX pad: before=%d afterInstall=%d afterStart=%d%s\n",
              (int)_mode, txBefore, txAfterInstall, txAfterStart,
              txAfterStart == 0 ? "   <-- CLAMPED DOMINANT (pad input on, so this is real)" : "");
    if (st != ESP_OK) { twai_driver_uninstall(); return false; }
    _installed = true;
    ++_installs;
    return true;
}

bool TwaiBus::begin(gpio_num_t txPin, gpio_num_t rxPin, uint32_t bitrate, twai_mode_t mode)
{
    if (_installed) end();
    _tx = txPin;
    _rxPin = rxPin;
    _bitrate = bitrate;
    _mode = mode;
    return install();
}

static void toFrame(const twai_message_t& m, Frame& out)
{
    out.id       = m.identifier;
    out.len      = m.data_length_code > 8 ? 8 : m.data_length_code;
    for (uint8_t i = 0; i < 8; ++i) out.data[i] = i < out.len ? m.data[i] : 0;
    out.extended = m.extd != 0;
    out.source   = Frame::SRC_MM_CAN;
}

bool TwaiBus::recvOne(Frame& out)
{
    if (!_installed) return false;
    if (_stashed) {
        out = _stash;
        _stashed = false;
        return true;
    }
    twai_message_t m{};
    if (twai_receive(&m, 0) != ESP_OK) return false;
    ++_rxCount;
    toFrame(m, out);
    return true;
}

void TwaiBus::waitRx(TickType_t ticks)
{
    if (!ticks) ticks = 1;
    if (!_installed) { vTaskDelay(ticks); return; }
    // A frame is already waiting to be read: nothing to sleep for.
    if (_stashed) return;
    twai_message_t m{};
    if (twai_receive(&m, ticks) != ESP_OK) return;
    ++_rxCount;
    toFrame(m, _stash);
    _stashed = true;
}

// twai_clear_transmit_queue() alone is not enough: it empties the software
// queue, but the frame already loaded into the controller stays there. With
// nobody to ACK it, an error-passive node does not count ACK errors any more,
// so it never reaches bus-off either: it retransmits that one frame for ever,
// ~1400 bus-error interrupts a second. twai_stop() puts the controller in
// reset mode, which aborts the frame and clears the queue; twai_start() brings
// it back on the same driver and config. The RX queue is cleared by the start,
// which costs nothing here: the gate only shuts after 2 s with nothing heard.
bool TwaiBus::abortTx()
{
    if (!_installed) return false;
    if (twai_stop() != ESP_OK) {
        // Not running (bus-off, recovering): nothing is being retransmitted,
        // and maintain() owns the way back.
        twai_clear_transmit_queue();
        return false;
    }
    if (twai_start() == ESP_OK) return true;
    // A controller that will not start again gets the full reinstall.
    return restart();
}

void TwaiBus::end()
{
    _stashed = false;
    if (!_installed) return;
    twai_stop();
    twai_driver_uninstall();
    _installed = false;
}

bool TwaiBus::restart()
{
    end();
    return install();
}

bool TwaiBus::running() const
{
    if (!_installed) return false;
    twai_status_info_t s{};
    return twai_get_status_info(&s) == ESP_OK && s.state == TWAI_STATE_RUNNING;
}

// Bus-off recovery without draining. The display link pulls via recvOne() and
// never calls poll(), so recovery must live outside poll(); a drain here would
// steal its frames.
void TwaiBus::maintain()
{
    if (!_installed) return;
    twai_status_info_t s{};
    if (twai_get_status_info(&s) != ESP_OK) return;
    if (s.state == TWAI_STATE_BUS_OFF) {
        twai_initiate_recovery();
    } else if (s.state == TWAI_STATE_STOPPED) {
        // Recovery completes into STOPPED.
        if (twai_start() != ESP_OK) restart();
    }
}

void TwaiBus::poll()
{
    if (!_installed || _inPoll) return;
    _inPoll = true;

    maintain();

    // The frame waitRx() woke on goes first, in order.
    if (_stashed) {
        _stashed = false;
        if (_rx) _rx(_stash, _rxCtx);
    }

    twai_message_t m;
    while (twai_receive(&m, 0) == ESP_OK) {
        ++_rxCount;
        if (!_rx) continue;
        Frame f;
        f.id = m.identifier;
        f.extended = m.extd;
        f.source = Frame::SRC_MM_CAN;
        f.len = m.data_length_code > 8 ? 8 : m.data_length_code;
        for (uint8_t i = 0; i < f.len; ++i) f.data[i] = m.data[i];
        _rx(f, _rxCtx);
    }

    _inPoll = false;
}

TwaiBus::TxResult TwaiBus::trySend(const Frame& f)
{
    if (!_installed || !_txEnabled || _mode == TWAI_MODE_LISTEN_ONLY)
        return TxResult::Failed;

    twai_message_t m = {};
    m.identifier = f.id;
    m.extd = f.extended;
    m.data_length_code = f.len > 8 ? 8 : f.len;
    for (uint8_t i = 0; i < m.data_length_code; ++i) m.data[i] = f.data[i];

    // Tap fires on the offer, including frames the controller refuses.
    if (_txTap) _txTap(f);

    // Zero timeout: never block the task that owns the display deadlines.
    // ESP_ERR_TIMEOUT then means "queue full now", distinct from a refusal.
    const esp_err_t e = twai_transmit(&m, 0);
    if (e == ESP_OK)             { ++_txOk;   return TxResult::Ok; }
    if (e == ESP_ERR_TIMEOUT)    { ++_txBusy; return TxResult::Busy; }
    ++_txFail;
    return TxResult::Failed;
}

bool TwaiBus::send(const Frame& f)
{
    // Busy collapses to false; only the display link uses trySend() directly.
    return trySend(f) == TxResult::Ok;
}
