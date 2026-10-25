#include "../utils/LogBus.h"
#include "KnobService.h"
#if FEATURE_KNOBS
#include "../board/BoardProfile.h"
#include "../app/Rt.h"
#include <Wire.h>
#include <driver/i2c.h>
#include <esp_log.h>

// Shared pins fail silently (encoder contact read as a held key, LED data on the
// head unit's KEY line), so the clash is a compile error.
#if FEATURE_MATRIX
static_assert(!Board::knobsClashWithMatrix(),
              "FEATURE_KNOBS and FEATURE_MATRIX share pins on this board — see BoardProfile.h");
#endif
#if FEATURE_RESISTIVE_KEYS
static_assert(!Board::knobsClashWithLadder(),
              "FEATURE_KNOBS and FEATURE_RESISTIVE_KEYS share pins on this board — see BoardProfile.h");
#endif

namespace Knobs
{
    static constexpr uint8_t  FRAME_MS     = 20;     // pot + lights: 50 Hz
    // Periodic unsolicited frame, so a corrupted bit does not stick until the next touch.
    static constexpr uint16_t REFRESH_MS   = 2000;

    void Service::begin(const Config& c, PostFn post, void* ctx)
    {
        _cfg = c;
        _cfg.sanitize();
        _post = post;
        _postCtx = ctx;
        // [mem] marks, serial only: begin() measured 9.5 kB, more than the task
        // and queue explain.
        auto mem = [](const char* what) {
            LogBus::printf("[mem] knobs.%-6s free=%6u\n", what, (unsigned)ESP.getFreeHeap());
        };
        mem("start");
        _lock = xSemaphoreCreateMutex();

        // Pull-up per pin: 34-39 have none and rely on the module's 10k.
        auto in = [](int8_t p) {
            if (p >= 0) pinMode(p, Board::hasPullup(p) ? INPUT_PULLUP : INPUT);
        };
        if (Board::KNOBS_I2C) {
            // 100 kHz: the expander sits a cable length away (and a PCF8574 is a
            // 100 kHz part). One read is ~0.2 ms of bus per 1 ms tick. Wire only
            // installs the driver; the transfers go to the IDF (probe()).
            _i2cOk = Wire.begin(Board::PIN_I2C_SDA, Board::PIN_I2C_SCL, 100000);
            // A stuck bus must not cost a line per failed transfer either.
            esp_log_level_set("i2c", ESP_LOG_NONE);
            if (!_i2cOk)
                LogBus::printf("[knobs] I2C on SDA=GPIO%d SCL=GPIO%d did not start: knobs off",
                               (int)Board::PIN_I2C_SDA, (int)Board::PIN_I2C_SCL);
            else if (findExpander())
                LogBus::printf("[knobs] PCF8574 at 0x%02X on SDA=GPIO%d SCL=GPIO%d", _expAddr,
                               (int)Board::PIN_I2C_SDA, (int)Board::PIN_I2C_SCL);
            else
                LogBus::printf("[knobs] NO PCF8574 on SDA=GPIO%d SCL=GPIO%d (0x20-27, 0x38-3F)%s; "
                               "searching in the background", (int)Board::PIN_I2C_SDA,
                               (int)Board::PIN_I2C_SCL, _busStuck ? ", BUS STUCK LOW" : "");
        } else {
            in(Board::PIN_KNOB_S1);
            in(Board::PIN_KNOB_S2);
            in(Board::PIN_KNOB_KEY);
            if (Board::hasKnob2()) {
                in(Board::PIN_KNOB2_S1);
                in(Board::PIN_KNOB2_S2);
                in(Board::PIN_KNOB2_KEY);
            }
        }
        // The pot pin is attached by analogRead() on first use.

        _strip.begin(Board::PIN_KNOB_LED);
        mem("strip");

        _src.begin(emitThunk, this);
        _src.configure(_cfg);
        _lights.configure(_cfg);
        mem("lights");

        // Own task, above the loop and pinned beside it: waiting on loop() drops
        // detents during repaints. Numbers and the reasons for them: TaskPlan.h.
        // Critical: a sampler that stops is dead knobs, which fails the OTA verdict.
        App::TaskSpec spec;
        spec.name     = "knobs";
        spec.fn       = taskThunk;
        spec.arg      = this;
        spec.stack    = Board::KNOBS_I2C ? App::Plan::KNOBS_STACK_I2C : App::Plan::KNOBS_STACK_GPIO;
        spec.prio     = App::Plan::KNOBS_PRIO;
        spec.core     = App::Plan::KNOBS_CORE;
        spec.critical = true;
        spec.idOut    = &_regId;
        _task = App::spawn(spec);
        mem("task");
    }

    void Service::emitThunk(const Keys::Event& e, bool pair, void* self)
    {
        Service* s = static_cast<Service*>(self);
        if (!s->_post || !s->_post(e, pair, s->_postCtx)) ++s->_dropped;
    }

    Config Service::config()
    {
        if (_lock) xSemaphoreTake(_lock, portMAX_DELAY);
        const Config c = _cfg;
        if (_lock) xSemaphoreGive(_lock);
        return c;
    }

    void Service::setConfig(const Config& c)
    {
        if (_lock) xSemaphoreTake(_lock, portMAX_DELAY);
        _cfg = c;
        _cfg.sanitize();
        _dirty = true;
        if (_lock) xSemaphoreGive(_lock);
    }

    Service::Live Service::live()
    {
        // Unlocked: single-word fields, one writer; one sample stale is fine.
        Live l{};
        l.encDetents = _src.encoder(ENC_RIGHT).detents();
        l.encErrors  = _src.encoder(ENC_RIGHT).errors();
        l.clicks     = _src.clicks(ENC_RIGHT);
        l.holds      = _src.holds(ENC_RIGHT);
        l.enc2Detents = _src.encoder(ENC_LEFT).detents();
        l.enc2Errors  = _src.encoder(ENC_LEFT).errors();
        l.clicks2     = _src.clicks(ENC_LEFT);
        l.holds2      = _src.holds(ENC_LEFT);
        l.btn2        = _src.buttonDown(ENC_LEFT);
        l.potSteps   = _src.potSteps();
        l.dropped    = _dropped;
        l.frames     = _strip.frames();
        l.busy       = _strip.busy();
        l.mA         = _lights.milliamps();
        l.mAWanted   = _lights.milliampsWanted();
        l.leds       = _lights.count();
        l.potRaw     = _src.pot().raw();
        l.potLevel   = _src.pot().level();
        l.potZone    = _src.pot().zone();
        l.master     = _lights.master();
        l.btn        = _src.buttonDown(ENC_RIGHT);
        l.stripOk    = _strip.ok();
        l.expAddr    = _expAddr;
        l.expErrors  = _expErrors;
        l.lines      = _lines;
        for (uint8_t i = 0; i < 6; ++i) l.edges[i] = _edges[i];
        l.pos[0] = _pos[ENC_RIGHT]; l.pos[1] = _pos[ENC_LEFT];
        return l;
    }

    // ---- the PCF8574 --------------------------------------------------------
    // Wire is only the driver's installer here. Its requestFrom() runs
    // log_e("i2cRead returned Error") on every failed read, and that line spins
    // ~6 ms on the UART0 console at priority 6 on core 1 — per sample, for as
    // long as the cable is out. The IDF calls below log nothing (the "i2c" tag
    // is silenced in begin()) and are bounded by I2C_WAIT.
    static constexpr i2c_port_t I2C_PORT = I2C_NUM_0;      // Wire = TwoWire(0)
    // Two ticks, not one: a one-tick timeout can expire at the next tick
    // boundary in the middle of a healthy 0.2 ms read. So a stuck bus costs
    // 1-2 ms per transfer, a missing device (no ACK) ~0.1 ms.
    static const TickType_t I2C_WAIT = pdMS_TO_TICKS(2) ? pdMS_TO_TICKS(2) : 1;
    // Failed reads in a row before the expander is taken as gone.
    static constexpr uint8_t  EXP_MISS_MAX   = 5;
    // Between sweeps of the 16 addresses while nothing answers; doubled up to
    // the max while the bus is stuck low (each probe then waits out I2C_WAIT).
    static constexpr uint16_t EXP_RETRY_MS   = 1000;
    static constexpr uint16_t EXP_RETRY_MAX  = 8000;
    static constexpr uint16_t LOG_GAP_MS     = 1000;   // a flapping cable: one line a second
    static constexpr uint8_t  EXP_ADDRS      = 16;
    static inline uint8_t expAddrAt(uint8_t i) { return (uint8_t)((i < 8 ? 0x20 : 0x30) + i); }

    // Writes 0xFF: a PCF8574 pin reads its input only while its output latch
    // is high (quasi-bidirectional), and power-up already leaves it so — the
    // write makes it true after a brown-out of the expander alone too.
    int8_t Service::probe(uint8_t addr)
    {
        const uint8_t ff = 0xFF;
        const esp_err_t e = i2c_master_write_to_device(I2C_PORT, addr, &ff, 1, I2C_WAIT);
        if (e == ESP_OK) return 1;
        return e == ESP_ERR_TIMEOUT ? -1 : 0;
    }

    bool Service::logDue(uint32_t nowMs)
    {
        if (_logAt && (uint32_t)(nowMs - _logAt) < LOG_GAP_MS) { ++_logSkipped; return false; }
        _logAt = nowMs ? nowMs : 1;
        return true;
    }

    // Setup: the whole sweep at once (~2 ms with nothing there). A timeout
    // ends it: the bus is stuck and every other address would wait it out too.
    bool Service::findExpander()
    {
        _busStuck = false;
        for (uint8_t i = 0; i < EXP_ADDRS; ++i) {
            const int8_t r = probe(expAddrAt(i));
            if (r > 0) { _expAddr = expAddrAt(i); return true; }
            if (r < 0) { _busStuck = true; break; }
        }
        _expAddr = 0;
        _probeIdx = 0;
        _expBackoffMs = _busStuck ? 2 * EXP_RETRY_MS : EXP_RETRY_MS;
        _expRetryAt = millis() + _expBackoffMs;
        return false;
    }

    // Sampler: one address per 1 ms tick, so a search never costs a tick more
    // than one probe — the LED frames and the 1 kHz cadence keep going.
    void Service::searchStep(uint32_t nowMs)
    {
        if (_probeIdx == 0 && (int32_t)(nowMs - _expRetryAt) < 0) return;
        const uint8_t a = expAddrAt(_probeIdx);
        const int8_t  r = probe(a);
        if (r > 0) {
            _expAddr = a;
            _expMiss = 0;
            _probeIdx = 0;
            _expBackoffMs = EXP_RETRY_MS;
            _busStuck = false;
            // History from before the outage is not a turn or a press now.
            _src.reseed();
            if (logDue(nowMs)) {
                LogBus::printf("[knobs] PCF8574 found at 0x%02X (%u failed reads since boot, %u lines held back)",
                               a, (unsigned)_expErrors, (unsigned)_logSkipped);
                _logSkipped = 0;
            }
            return;
        }
        if (r < 0) {
            // Stuck low: abandon this sweep and wait longer each time.
            if (!_busStuck && logDue(nowMs))
                LogBus::printf("[knobs] I2C bus stuck (SDA/SCL held low?); retrying in %u ms",
                               (unsigned)_expBackoffMs);
            _busStuck = true;
            _probeIdx = 0;
            _expRetryAt = nowMs + _expBackoffMs;
            _expBackoffMs = (uint16_t)(_expBackoffMs * 2 > EXP_RETRY_MAX ? EXP_RETRY_MAX : _expBackoffMs * 2);
            return;
        }
        if (++_probeIdx < EXP_ADDRS) return;
        // Nobody answered: the bus itself is fine, so the next sweep in a second.
        _probeIdx = 0;
        _busStuck = false;
        _expBackoffMs = EXP_RETRY_MS;
        _expRetryAt = nowMs + EXP_RETRY_MS;
    }

    bool Service::readLines(Lines& l, uint32_t nowMs)
    {
        if (Board::KNOBS_I2C) {
            if (!_i2cOk) return false;
            if (!_expAddr) { searchStep(nowMs); return false; }   // found = next tick's sample
            uint8_t v = 0xFF;
            if (i2c_master_read_from_device(I2C_PORT, _expAddr, &v, 1, I2C_WAIT) != ESP_OK) {
                ++_expErrors;
                // Several misses IN A ROW: assume it is gone and search again,
                // at once. A single miss is a skipped sample (QuadDecoder makes
                // up one missed quarter step).
                if (++_expMiss >= EXP_MISS_MAX) {
                    if (logDue(nowMs)) {
                        LogBus::printf("[knobs] PCF8574 0x%02X not answering (%u failed reads); searching",
                                       _expAddr, (unsigned)_expErrors);
                    }
                    _expAddr = 0;
                    _expMiss = 0;
                    _probeIdx = 0;
                    _expRetryAt = nowMs;
                }
                return false;
            }
            _expMiss = 0;
            // Active low: a closed contact shorts the line to ground.
            auto on = [v](int8_t bit) { return bit >= 0 && !(v & (1u << bit)); };
            l.a1 = on(Board::PCF_BIT_S1);  l.b1 = on(Board::PCF_BIT_S2);  l.k1 = on(Board::PCF_BIT_KEY);
            l.a2 = on(Board::PCF_BIT2_S1); l.b2 = on(Board::PCF_BIT2_S2); l.k2 = on(Board::PCF_BIT2_KEY);
            return true;
        }
        auto on = [](int8_t pin) { return pin >= 0 && digitalRead(pin) == LOW; };
        l.a1 = on(Board::PIN_KNOB_S1);  l.b1 = on(Board::PIN_KNOB_S2);  l.k1 = on(Board::PIN_KNOB_KEY);
        l.a2 = on(Board::PIN_KNOB2_S1); l.b2 = on(Board::PIN_KNOB2_S2); l.k2 = on(Board::PIN_KNOB2_KEY);
        return true;
    }

    void Service::taskThunk(void* self) { static_cast<Service*>(self)->run(); }

    void Service::run()
    {
        Config   c = config();
        uint32_t frameAt = 0, shownAt = 0;
        bool     everShown = false;
        uint8_t  sentMax = 0;

        TickType_t last = xTaskGetTickCount();
        for (;;) {
            const uint32_t now = millis();
            const uint32_t passAtUs = micros();

            // Try-take only: httpd (core 0, prio 2) holds the lock for a Config
            // copy and can be preempted there by WiFi/lwIP, which priority
            // inheritance does not outrank. Held = keep the old Config, _dirty
            // stays set, and the next tick tries again.
            if (_dirty && xSemaphoreTake(_lock, 0) == pdTRUE) {
                c = _cfg;
                _dirty = false;
                xSemaphoreGive(_lock);
                _src.configure(c);
                _lights.configure(c);
                everShown = false;          // a new order or count wants a frame
            }

            // One read of all six lines per tick (one I2C byte on an expander).
            // A failed read skips the sample rather than feeding the decoder
            // all-open contacts, which would look like a turn.
            Lines ln{};
            const bool have = (c.enc[ENC_RIGHT].enabled || c.enc[ENC_LEFT].enabled) && readLines(ln, now);
            if (have) {
                const uint8_t v = (uint8_t)(ln.a1 | ln.b1 << 1 | ln.k1 << 2 | ln.a2 << 3 | ln.b2 << 4 | ln.k2 << 5);
                for (uint8_t i = 0, x = v ^ _lines; x; ++i, x >>= 1)
                    if (x & 1) _edges[i] = _edges[i] + 1;
                _lines = v;
            }
            if (have && c.enc[ENC_RIGHT].enabled) {
                const int8_t d = _src.sampleEncoder(ENC_RIGHT, ln.a1, ln.b1, now);
                if (d) { _lights.onTurn(ZONE_RIGHT, d, now); _pos[ENC_RIGHT] = _pos[ENC_RIGHT] + d; }
                if (_src.sampleButton(ENC_RIGHT, ln.k1, now))
                    _lights.onButton(ZONE_RIGHT, now);
            }
            if (have && Board::hasKnob2() && c.enc[ENC_LEFT].enabled) {
                const int8_t d = _src.sampleEncoder(ENC_LEFT, ln.a2, ln.b2, now);
                if (d) { _lights.onTurn(ZONE_LEFT, d, now); _pos[ENC_LEFT] = _pos[ENC_LEFT] + d; }
                if (_src.sampleButton(ENC_LEFT, ln.k2, now))
                    _lights.onButton(ZONE_LEFT, now);
            }

            if ((uint32_t)(now - frameAt) >= FRAME_MS) {
                frameAt = now;

                // No pot pin, no ADC read: on the DevKit that pin is the left
                // encoder's button and analogRead() would re-mux it.
                if (Board::PIN_KNOB_POT >= 0)
                    _src.samplePot((uint16_t)analogRead(Board::PIN_KNOB_POT), now);
                if (Board::PIN_KNOB_POT >= 0 && c.potMode != PotMode::Off) {
                    _lights.setLevel(ZONE_LEFT, _src.pot().level(), now);
                    if (c.potMode == PotMode::Brightness) _lights.setMaster(_src.pot().level());
                }
                // The dash's dimmer under the set brightness, its ceiling
                // (Config::dashMaster); its absence: the brightness itself.
                if (c.dashFollow && _dash)
                    _lights.setMaster(Config::dashMaster(c.brightness, _dash(), c.dashGain));

                const bool changed = _lights.tick(now);
                if (changed || !everShown || (uint32_t)(now - shownAt) >= REFRESH_MS) {
                    // Dark must be sent once: the chips hold their last colour.
                    // Send as many pixels as were ever lit so a shrunk count goes dark.
                    if (_lights.count() > sentMax) sentMax = _lights.count();
                    // show() may refuse while the last frame is on the wire; a
                    // refused frame is not marked shown, so it is retried.
                    const bool wanted = c.ledEnabled || changed || !everShown;
                    if (!wanted || !sentMax || !_strip.ok()) {
                        shownAt = now;
                        everShown = true;
                    } else if (_strip.show(_lights.pixels(), sentMax, c.order)) {
                        shownAt = now;
                        everShown = true;
                    } else {
                        everShown = false;
                    }
                }
            }

            App::Tasks::beat(_regId, micros() - passAtUs);
            vTaskDelayUntil(&last, pdMS_TO_TICKS(1));
        }
    }
}
#endif
