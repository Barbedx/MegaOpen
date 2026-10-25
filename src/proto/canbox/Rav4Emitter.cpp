#include "Rav4Emitter.h"
#include "RaiseFramer.h"

namespace Canbox
{
    using namespace Vehicle;

    // Short local aliases for the public table in the header.
    using M = Rav4Emitter;
    enum : uint8_t {
        M_DOORS = M::MSG_DOORS,   M_TEMP = M::MSG_TEMP,
        M_AC = M::MSG_AC,         M_STEER = M::MSG_STEER,
        M_LIGHTS = M::MSG_LIGHTS, M_SPEED = M::MSG_SPEED,
        M_ODO = M::MSG_ODO,       M_RPM = M::MSG_RPM,
        M_TRIP = M::MSG_TRIP,     M_FUEL_INST = M::MSG_FUEL_INST,
        M_FUEL_HIST = M::MSG_FUEL_HIST,
        M_RADAR_REAR = M::MSG_RADAR_REAR, M_RADAR_FRONT = M::MSG_RADAR_FRONT,
        M_TPMS = M::MSG_TPMS,     M_SETTINGS = M::MSG_SETTINGS,
        M_AMP = M::MSG_AMP,       M_SYSTEM = M::MSG_SYSTEM,
        M_ILLUM = M::MSG_ILLUM,   M_VSET = M::MSG_VSET,
        M_SEATS = M::MSG_SEATS,   M_HYBRID = M::MSG_HYBRID,
        M_VERSION = M::MSG_VERSION, M_COOLANT = M::MSG_COOLANT,
        M_GEAR = M::MSG_GEAR,
        M_COUNT = M::MSG_COUNT
    };

    static_assert(M_COUNT <= Rav4Emitter::MAX_MESSAGES,
                  "Rav4Emitter::MAX_MESSAGES is smaller than the message table");

    static const char* const kNames[M_COUNT] = {
        "doors+ign 0x24", "out_temp 0x28", "aircon 0x28", "steering 0x29",
        "lights 0x7D01", "speed 0x7D03", "odometer 0x7D04", "rpm 0x7D0A",
        "trip+range 0x21", "fuel_inst 0x22", "fuel_hist 0x23",
        "radar_rear 0x1E", "radar_front 0x1D", "tpms 0x25", "settings 0x26",
        "amplifier 0x31", "system 0x32", "illum_delay 0x50", "vset 0x52",
        "seats 0x58", "hybrid 0x1F", "version 0x30", "coolant 0x7D09",
        "prnd 0x7D06",
    };

    static const uint8_t kCmd[M_COUNT] = {
        0x24, 0x28, 0x28, 0x29, 0x7D, 0x7D, 0x7D, 0x7D,
        0x21, 0x22, 0x23, 0x1E, 0x1D, 0x25, 0x26, 0x31,
        0x32, 0x50, 0x52, 0x58, 0x1F, 0x30, 0x7D, 0x7D,
    };

    static const uint32_t kInterval[M_COUNT] = {
        250, 5000, 1000, 200, 200, 500, 10000, 333,
        5000, 1000, 5000, 100, 100, 5000, 5000, 5000,
        5000, 10000, 5000, 5000, 1000, 10000, 5000, 1000,
    };

    // On-change messages go out as soon as their payload changes, not on the timer.
    static const bool kOnChange[M_COUNT] = {
        true, false, true, false, true, false, false, false,
        false, false, false, true, true, false, true, true,
        true, false, true, true, false, false, true, true,
    };

    // 0x21/0x22/0x23 are big-endian, unlike the 0x7D family.
    static inline void putBE16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

    // -5..+5 travels with a +7 offset, i.e. 0x02..0x0C.
    static inline uint8_t tone(int32_t v)
    {
        if (v < -5) v = -5;
        if (v >  5) v =  5;
        return (uint8_t)(v + 7);
    }
    static inline uint8_t nib(int32_t v, int32_t hi)
    {
        if (v < 0) v = 0;
        if (v > hi) v = hi;
        return (uint8_t)v;
    }

    // Radar distance with the polarity switch. 0 ("no display") is never mirrored.
    uint8_t Rav4Emitter::radar(int32_t v) const
    {
        const uint8_t d = nib(v, 4);
        if (!_t.radarInvert || d == 0) return d;
        return (uint8_t)(5 - d);
    }

    const char* Rav4Emitter::experimentsWhy(uint8_t m)
    {
        if (m & ~EXP_ALL) return "param exp";
        if (!(m & EXP_BATT)) return nullptr;
        const uint8_t at = (uint8_t)((m & EXP_BATT_AT) >> 5);
        if (at > (uint8_t)BattAt::Hist23) return "param exp";   // battery field: 0x21, 0x7D 04 or 0x23
        if (at == (uint8_t)BattAt::Odo7D04 && (m & EXP_FUEL_7D04))
            return "exp_7d04";    // battery and fuel both in 0x7D 04
        if (at != (uint8_t)BattAt::Odo7D04 && (m & EXP_COMPUTED))
            return "exp_range";   // battery in 0x21/0x23 and the computed range/consumption
        return nullptr;
    }

    uint8_t Rav4Emitter::sanitizeExperiments(uint8_t m)
    {
        m &= EXP_ALL;
        if (!(m & EXP_BATT)) return m;
        BattAt at = battAt(m);
        if ((uint8_t)at > (uint8_t)BattAt::Hist23) { at = BattAt::Range21; m = withBattAt(m, at); }
        if (at == BattAt::Odo7D04) m &= (uint8_t)~EXP_FUEL_7D04;
        else                       m &= (uint8_t)~EXP_COMPUTED;
        return m;
    }

    uint16_t Rav4Emitter::battField(int32_t v)
    {
        if (v <= 0) return 0;
        const int32_t x = (v + 5) / 10;
        return (uint16_t)(x > 0xFFFF ? 0xFFFF : x);
    }

    uint8_t Rav4Emitter::pick(const State& s, uint8_t sig, uint8_t computed) const
    {
        if (s.has(sig)) return sig;
        if ((_exp & EXP_COMPUTED) && s.has(computed)) return computed;
        return SIG_COUNT;
    }

    // ---- Tuning persistence -------------------------------------------------
    // Bytes 7..10 of a blob written since the doorsLen migration. Read with the
    // OLD layout (rpmMul = 7|8<<8, rpmDiv = 9|10<<8) they are 840 / 210 = x4,
    // so an OTA rollback to an older build still sends the right rpm.
    static constexpr uint8_t DOORS_MIGRATED[4] = {0x48, 0x03, 0xD2, 0x00};
    // A fixed 32-byte blob, byte 0 = layout version (1). Any other version is
    // rejected, never reinterpreted.
    void Tuning::toBlob(uint8_t* out) const
    {
        for (uint8_t i = 0; i < BLOB_SIZE; ++i) out[i] = 0;
        out[0]  = 1;                       // layout version
        out[1]  = cmd28;
        out[2]  = doorsLen;
        out[3]  = steerInvert;
        out[4]  = steerDiv;
        out[5]  = (uint8_t)(steerClamp & 0xFF);
        out[6]  = (uint8_t)((steerClamp >> 8) & 0xFF);
        // 7..10: the old rpm scale (now fixed at x4), reused as the migration
        // marker; see fromBlob.
        for (uint8_t i = 0; i < 4; ++i) out[7 + i] = DOORS_MIGRATED[i];
        out[11] = fuelUnit;
        out[12] = tripUnit;
        out[13] = tripFields;
        out[14] = histLen;
        out[15] = radarInvert;
        for (uint8_t i = 0; i < 16; ++i) out[16 + i] = (uint8_t)version[i];
    }

    bool Tuning::fromBlob(const uint8_t* in, uint8_t len)
    {
        if (!in || len != BLOB_SIZE || in[0] != 1) return false;
        cmd28       = in[1];
        doorsLen    = (in[2] == 2) ? 2 : 1;
        // ONE-TIME MIGRATION. len 1 was the default until 2026-10-17 and is known
        // bad: the head unit reads D1 whatever the length, so it shows the
        // bonnet open (docs/hu-decoder.md §2a). A blob from before then holds
        // 1 because it was the default, and becomes 2. Bytes 7..10 equal to
        // DOORS_MIGRATED mark a blob written since: there a 1 is the owner's
        // own choice and stays.
        const bool migrated = in[7] == DOORS_MIGRATED[0] && in[8] == DOORS_MIGRATED[1] &&
                              in[9] == DOORS_MIGRATED[2] && in[10] == DOORS_MIGRATED[3];
        if (!migrated && doorsLen == 1) doorsLen = 2;
        steerInvert = in[3];
        steerDiv    = in[4] ? in[4] : 10;
        steerClamp  = (int16_t)(in[5] | (in[6] << 8));
        fuelUnit    = in[11];
        tripUnit    = in[12];
        tripFields  = in[13];
        // Out-of-range length: fall back to the spec value.
        histLen     = (in[14] >= 3 && in[14] <= 13) ? in[14] : 13;
        radarInvert = in[15];
        for (uint8_t i = 0; i < 16; ++i) version[i] = (char)in[16 + i];
        version[16] = '\0';
        return true;
    }

    Rav4Emitter::Rav4Emitter()
    {
        for (uint8_t i = 0; i < MAX_MESSAGES; ++i) _msgOn[i] = true;
    }

    uint8_t Rav4Emitter::messageCount() { return M_COUNT; }
    const char* Rav4Emitter::messageName(uint8_t i) { return i < M_COUNT ? kNames[i] : "?"; }
    uint8_t Rav4Emitter::messageCmd(uint8_t i) { return i < M_COUNT ? kCmd[i] : 0; }
    uint32_t Rav4Emitter::messageInterval(uint8_t i) { return i < M_COUNT ? kInterval[i] : 0; }

    void Rav4Emitter::setMsgEnabled(uint8_t i, bool on) { if (i < M_COUNT) _msgOn[i] = on; }
    bool Rav4Emitter::msgEnabled(uint8_t i) const { return i < M_COUNT && _msgOn[i]; }

    // By side, not role: b7 right-front, b6 left-front, b5 right-rear, b4
    // left-rear, b3 tailgate. "driver = 0x80" only holds in a RHD car
    // (RAV4_BODY_ENGINE_2018_2020.pdf §4: b7 "Driver (UK right front)").
    uint8_t Rav4Emitter::doorsMask(const State& s)
    {
        uint8_t m = 0;
        if (s.get(SIG_DOOR_FR)) m |= 0x80;
        if (s.get(SIG_DOOR_FL)) m |= 0x40;
        if (s.get(SIG_DOOR_RR)) m |= 0x20;
        if (s.get(SIG_DOOR_RL)) m |= 0x10;
        if (s.get(SIG_BOOT))    m |= 0x08;
        return m;
    }

    uint8_t Rav4Emitter::lightsMask(const State& s)
    {
        uint8_t m = 0;
        if (s.get(SIG_LIGHT_POS))    m |= 0x80;
        if (s.get(SIG_LIGHT_DIPPED)) m |= 0x40;
        if (s.get(SIG_LIGHT_MAIN))   m |= 0x20;
        if (s.get(SIG_IND_LEFT))     m |= 0x10;
        if (s.get(SIG_IND_RIGHT))    m |= 0x08;
        return m;
    }

    void Rav4Emitter::emit(uint8_t cmd, const uint8_t* payload, uint8_t len)
    {
        if (!_write) return;
        uint8_t frame[MAX_FRAME];
        uint8_t n = buildFrame(cmd, payload, len, frame);
        if (!n) return;
        _write(frame, n, _ctx);
        ++_framesSent;
        if (cmd == 0x28) {
            uint32_t h = 2166136261u;
            h = (h ^ len) * 16777619u;
            for (uint8_t b = 0; b < len; ++b) h = (h ^ payload[b]) * 16777619u;
            if (!h) h = 1;
            if (_last28 && h != _last28) ++_climateChanges;
            _last28 = h;
        }
    }

    bool Rav4Emitter::due(Sched& sc, uint32_t nowMs, uint32_t interval,
                          uint32_t value, bool onChange)
    {
        const bool timeUp  = sc.lastMs == 0 || (uint32_t)(nowMs - sc.lastMs) >= interval;
        const bool changed = onChange && value != sc.lastVal && sc.lastVal != 0xFFFFFFFF;
        if (!timeUp && !changed) return false;
        sc.lastMs  = nowMs ? nowMs : 1;
        sc.lastVal = value;
        return true;
    }

    // False when no signal is decoded or pinned: silence, never fabricated zeros.
    // Composite messages go out if ANY of their signals is known; unknown
    // fields are 0.
    bool Rav4Emitter::build(uint8_t msgIndex, const State& s,
                            uint8_t& cmd, uint8_t* p, uint8_t& len) const
    {
        switch (msgIndex) {
            case M_DOORS: {
                if (!(s.has(SIG_DOOR_FL) || s.has(SIG_DOOR_FR) || s.has(SIG_DOOR_RL) ||
                      s.has(SIG_DOOR_RR) || s.has(SIG_BOOT) || s.has(SIG_KEY_ON) ||
                      s.has(SIG_BONNET))) return false;
                cmd = 0x24;
                p[0] = doorsMask(s);
                if (_t.doorsLen == 2) {
                    // D1 b1 = ignition; gates the trip/fuel/history/TPMS panels
                    // (vendor spec). See setDoorsWithIgnition(). D1 b7 = bonnet
                    // (docs/hu-decoder.md §2a); len 1 has no D1 to carry it.
                    len = 2;
                    p[1] = (uint8_t)((s.get(SIG_KEY_ON) ? 0x02 : 0x00) | (s.get(SIG_BONNET) ? 0x80 : 0x00));
                } else {
                    len = 1;
                }
                return true;
            }
            case M_TEMP: {
                if (!s.has(SIG_OUT_TEMP)) return false;
                cmd = 0x28; len = 12;
                for (uint8_t i = 0; i < 12; ++i) p[i] = 0;
                // The held value (TempHold.h), so a flickering reading does not
                // change the climate frame; a pinned test value goes at once.
                const int32_t raw = s.get(SIG_OUT_TEMP);
                int32_t t = State::clamp(SIG_OUT_TEMP, s.pinned(SIG_OUT_TEMP) || !_temp.have() ? raw : _temp.shown());
                p[5] = (uint8_t)((t + 40) * 2);
                return true;
            }
            case M_AC: {
                static const uint8_t kAcSigs[] = {
                    SIG_AC_ON, SIG_AC_COMPRESSOR, SIG_AC_RECIRC, SIG_AC_AUTO_HI,
                    SIG_AC_AUTO_LO, SIG_AC_DUAL, SIG_AC_MAX_FRONT, SIG_AC_REAR,
                    SIG_AC_VENT_UP, SIG_AC_VENT_FACE, SIG_AC_VENT_FLOOR,
                    SIG_AC_DISPLAY_REQ, SIG_AC_FAN, SIG_AC_TEMP_DRV,
                    SIG_AC_TEMP_PASS, SIG_AC_DEFOG, SIG_AC_AQS, SIG_AC_ECO,
                    SIG_AC_UNIT_F,
                };
                bool any = false;
                for (uint8_t i = 0; i < sizeof(kAcSigs) && !any; ++i) any = s.has(kAcSigs[i]);
                if (!any || !acTrusted(s)) return false;

                cmd = 0x28; len = 5;
                p[0] = (uint8_t)((s.get(SIG_AC_ON)         ? 0x80 : 0) |
                                 (s.get(SIG_AC_COMPRESSOR) ? 0x40 : 0) |
                                 (s.get(SIG_AC_RECIRC)     ? 0x20 : 0) |
                                 (s.get(SIG_AC_AUTO_HI)    ? 0x10 : 0) |
                                 (s.get(SIG_AC_AUTO_LO)    ? 0x08 : 0) |
                                 (s.get(SIG_AC_DUAL)       ? 0x04 : 0) |
                                 (s.get(SIG_AC_MAX_FRONT)  ? 0x02 : 0) |
                                 (s.get(SIG_AC_REAR)       ? 0x01 : 0));
                p[1] = (uint8_t)((s.get(SIG_AC_VENT_UP)     ? 0x80 : 0) |
                                 (s.get(SIG_AC_VENT_FACE)   ? 0x40 : 0) |
                                 (s.get(SIG_AC_VENT_FLOOR)  ? 0x20 : 0) |
                                 (s.get(SIG_AC_DISPLAY_REQ) ? 0x10 : 0) |
                                 nib(s.get(SIG_AC_FAN), 7));
                // Raw wire bytes: the sources disagree on the setpoint
                // encoding, so the conversion lives in the UI.
                p[2] = (uint8_t)nib(s.get(SIG_AC_TEMP_DRV), 255);
                p[3] = (uint8_t)nib(s.get(SIG_AC_TEMP_PASS), 255);
                p[4] = (uint8_t)((s.get(SIG_AC_DEFOG)  ? 0x80 : 0) |
                                 (s.get(SIG_AC_AQS)    ? 0x40 : 0) |
                                 (s.get(SIG_AC_ECO)    ? 0x20 : 0) |
                                 (s.get(SIG_AC_UNIT_F) ? 0x01 : 0));
                return true;
            }
            case M_STEER: {
                if (!s.has(SIG_STEERING)) return false;
                // Signed 16-bit, whole degrees, LSB first, upper byte
                // sign-extended. Vendor spec: 12-bit field, D1 b7-4 ignored.
                // mikescotland: -26 deg -> 2E 29 02 E6 FF EF (checksum verifies,
                // full int16). Sign-extending satisfies both decoders; masking
                // to 12 bits breaks every left-hand angle for an int16 reader.
                const int16_t div = _t.steerDiv ? _t.steerDiv : 1;
                int32_t deg = s.get(SIG_STEERING) / div;
                if (_t.steerInvert) deg = -deg;
                const int32_t lim = _t.steerClamp;
                if (deg < -lim) deg = -lim;
                if (deg >  lim) deg =  lim;
                const uint16_t raw = (uint16_t)(int16_t)deg;
                cmd = 0x29; len = 2;
                p[0] = (uint8_t)(raw & 0xFF);
                p[1] = (uint8_t)(raw >> 8);
                return true;
            }
            case M_LIGHTS: {
                if (!(s.has(SIG_LIGHT_POS) || s.has(SIG_LIGHT_DIPPED) || s.has(SIG_LIGHT_MAIN) ||
                      s.has(SIG_IND_LEFT) || s.has(SIG_IND_RIGHT))) return false;
                cmd = 0x7D; len = 2; p[0] = 0x01; p[1] = lightsMask(s);
                return true;
            }
            case M_SPEED: {
                if (!s.has(SIG_SPEED)) return false;
                uint32_t v = (uint32_t)s.get(SIG_SPEED);      // km/h x100
                if (v > 0xFFFF) v = 0xFFFF;
                // The trailing pair is average speed, not padding.
                uint32_t avg = (uint32_t)s.get(SIG_AVG_SPEED);
                if (avg > 0xFFFF) avg = 0xFFFF;
                cmd = 0x7D; len = 5;
                p[0] = 0x03;
                p[1] = (uint8_t)(v & 0xFF);   p[2] = (uint8_t)(v >> 8);
                p[3] = (uint8_t)(avg & 0xFF); p[4] = (uint8_t)(avg >> 8);
                return true;
            }
            case M_ODO: {
                // EXP_FUEL_7D04 also takes the cluster's odometer (0x5C5) when
                // 0x5FD is silent: the X84 matrix has no 0x5FD.
                const bool fuelExp = (_exp & EXP_FUEL_7D04) != 0;
                const bool battExp = (_exp & EXP_BATT) && battAt(_exp) == BattAt::Odo7D04;
                const uint8_t odo = s.has(SIG_ODOMETER) ? SIG_ODOMETER
                                  : (fuelExp || battExp) && s.has(SIG_ODO_CLUSTER) ? SIG_ODO_CLUSTER : SIG_COUNT;
                if (odo == SIG_COUNT) return false;
                uint32_t km = (uint32_t)s.get(odo);
                cmd = 0x7D; len = 12;
                p[0] = 0x04;
                p[1] = (uint8_t)(km & 0xFF);
                p[2] = (uint8_t)((km >> 8) & 0xFF);
                p[3] = (uint8_t)((km >> 16) & 0xFF);
                p[4] = 0xF2; p[5] = 0x08;    // in every capture: 2290, /10 -> 1049 idx 5, meaning unknown
                if (fuelExp && s.has(SIG_FUEL_LEVEL)) {
                    const uint16_t f = (uint16_t)(s.get(SIG_FUEL_LEVEL) * 10);   // litres x10, LE
                    p[4] = (uint8_t)f; p[5] = (uint8_t)(f >> 8);
                }
                if (battExp && s.has(SIG_BATT_V)) {
                    const uint16_t b = battField(s.get(SIG_BATT_V));            // V x10, LE
                    p[4] = (uint8_t)b; p[5] = (uint8_t)(b >> 8);
                }
                // trip1/trip2, LE u24 each, in 0.1 km.
                uint32_t t1 = (uint32_t)s.get(SIG_TRIP1);
                uint32_t t2 = (uint32_t)s.get(SIG_TRIP2);
                p[6] = (uint8_t)(t1 & 0xFF); p[7] = (uint8_t)((t1 >> 8) & 0xFF); p[8] = (uint8_t)((t1 >> 16) & 0xFF);
                p[9] = (uint8_t)(t2 & 0xFF); p[10] = (uint8_t)((t2 >> 8) & 0xFF); p[11] = (uint8_t)((t2 >> 16) & 0xFF);
                return true;
            }
            case M_RPM: {
                if (!s.has(SIG_RPM)) return false;
                // rpm x4 on the wire (2500 rpm -> 10000 -> 2E 7D 03 0A 10 27 3E),
                // confirmed by two sources; the head unit's own parser passes it
                // on unscaled (docs/hu-decoder.md §2b). Any scale on the car's
                // side belongs in the decode row (raw / 8 on the Megane).
                int32_t r = s.get(SIG_RPM) * 4;
                if (r < 0) r = 0;
                if (r > 0xFFFF) r = 0xFFFF;
                cmd = 0x7D; len = 3;
                p[0] = 0x0A; p[1] = (uint8_t)(r & 0xFF); p[2] = (uint8_t)(r >> 8);
                return true;
            }
            case M_TRIP: {
                const uint8_t range = pick(s, SIG_RANGE, SIG_RANGE_CALC);
                const bool batt = (_exp & EXP_BATT) && battAt(_exp) == BattAt::Range21 && s.has(SIG_BATT_V);
                if (!(s.has(SIG_AVG_SPEED) || s.has(SIG_TRIP_ELAPSED) || range != SIG_COUNT || batt))
                    return false;
                // Big-endian; elapsed is in minutes, not seconds.
                // mikescotland: p[0..3] zero padding, p[4..5] range (BE16), p[6]
                // constant 0x02. Vendor spec: p[0..1] average speed, p[2..3]
                // elapsed minutes. Filling p[0..3] is a superset of both.
                cmd = 0x21; len = 7;
                if (_t.tripFields) {
                    putBE16(p + 0, (uint16_t)(s.get(SIG_AVG_SPEED) / 10));   // x100 -> x10
                    putBE16(p + 2, (uint16_t)s.get(SIG_TRIP_ELAPSED));
                } else {
                    putBE16(p + 0, 0);
                    putBE16(p + 2, 0);
                }
                putBE16(p + 4, batt ? battField(s.get(SIG_BATT_V))
                                    : (uint16_t)(range != SIG_COUNT ? s.get(range) : 0));
                p[6] = _t.tripUnit;
                return true;
            }
            case M_FUEL_INST: {
                const uint8_t inst = pick(s, SIG_FUEL_INST, SIG_CONS_INST);
                if (inst == SIG_COUNT) return false;
                cmd = 0x22; len = 3;
                p[0] = _t.fuelUnit;
                putBE16(p + 1, (uint16_t)s.get(inst));          // x10
                return true;
            }
            case M_FUEL_HIST: {
                const uint8_t avg = pick(s, SIG_FUEL_AVG, SIG_CONS_AVG);
                const bool batt = (_exp & EXP_BATT) && battAt(_exp) == BattAt::Hist23 && s.has(SIG_BATT_V);
                if (avg == SIG_COUNT && !batt) return false;
                // len 13, not 3: the unit's exact-length guard drops 3.
                // Unused slots are 0xFFFF = invalid, filled from the end.
                cmd = 0x23; len = _t.histLen ? _t.histLen : 13;
                p[0] = _t.fuelUnit;
                putBE16(p + 1, batt ? battField(s.get(SIG_BATT_V))   // the borrowed bar
                                    : (uint16_t)s.get(avg));                 // current trip
                for (uint8_t i = 3; i + 1 < len; i += 2) putBE16(p + i, 0xFFFF);
                return true;
            }
            case M_RADAR_REAR: {
                if (!(s.has(SIG_RADAR_RL) || s.has(SIG_RADAR_RLM) || s.has(SIG_RADAR_RRM) ||
                      s.has(SIG_RADAR_RR) || s.has(SIG_RADAR_ENABLE))) return false;
                cmd = 0x1E; len = 5;
                p[0] = radar(s.get(SIG_RADAR_RL));
                p[1] = radar(s.get(SIG_RADAR_RLM));
                p[2] = radar(s.get(SIG_RADAR_RRM));
                p[3] = radar(s.get(SIG_RADAR_RR));
                // b5 is the radar master enable, stored by the head unit; every
                // 0x1D is discarded while it is clear. Hence rear precedes
                // front in the table.
                p[4] = (uint8_t)((s.get(SIG_RADAR_DISPLAY) ? 0x80 : 0) |
                                 (s.get(SIG_RADAR_FAR)     ? 0x40 : 0) |
                                 (s.get(SIG_RADAR_ENABLE)  ? 0x20 : 0) |
                                 nib(s.get(SIG_RADAR_VOLUME), 5));
                return true;
            }
            case M_RADAR_FRONT: {
                if (!(s.has(SIG_RADAR_FL) || s.has(SIG_RADAR_FLM) || s.has(SIG_RADAR_FRM) ||
                      s.has(SIG_RADAR_FR))) return false;
                // Right to left: mirrored relative to 0x1E, as documented.
                cmd = 0x1D; len = 4;
                p[0] = radar(s.get(SIG_RADAR_FR));
                p[1] = radar(s.get(SIG_RADAR_FRM));
                p[2] = radar(s.get(SIG_RADAR_FLM));
                p[3] = radar(s.get(SIG_RADAR_FL));
                return true;
            }
            case M_TPMS: {
                if (!s.has(SIG_TPMS_PRESENT)) return false;
                cmd = 0x25; len = 6;
                p[0] = (uint8_t)((s.get(SIG_TPMS_PRESENT)   ? 0x80 : 0) |
                                 (s.get(SIG_TPMS_ALARM)     ? 0x40 : 0) |
                                 (s.get(SIG_TPMS_SHOW_SPARE)? 0x20 : 0) |
                                 (s.get(SIG_TPMS_DIAGRAM)   ? 0x04 : 0) |
                                 nib(s.get(SIG_TPMS_UNIT), 3));
                // 0xFF = no data for this wheel (0 would read as a flat tyre).
                p[1] = s.has(SIG_TPMS_FL)    ? (uint8_t)s.get(SIG_TPMS_FL)    : 0xFF;
                p[2] = s.has(SIG_TPMS_FR)    ? (uint8_t)s.get(SIG_TPMS_FR)    : 0xFF;
                p[3] = s.has(SIG_TPMS_RL)    ? (uint8_t)s.get(SIG_TPMS_RL)    : 0xFF;
                p[4] = s.has(SIG_TPMS_RR)    ? (uint8_t)s.get(SIG_TPMS_RR)    : 0xFF;
                p[5] = s.has(SIG_TPMS_SPARE) ? (uint8_t)s.get(SIG_TPMS_SPARE) : 0xFF;
                return true;
            }
            case M_SETTINGS: {
                if (!(s.has(SIG_SET_DRL) || s.has(SIG_SET_LAMP_SENS) ||
                      s.has(SIG_SET_ROOM_DELAY) || s.has(SIG_SET_LAMP_AUTOOFF) ||
                      s.has(SIG_SET_LOCK1) || s.has(SIG_SET_LOCK2) ||
                      s.has(SIG_SET_GUIDELINE) || s.has(SIG_SET_RELOCK))) return false;
                // Never steering angle here: 0x26 is the angle on the VW profile,
                // but the settings block on this one; an angle corrupts the menu.
                cmd = 0x26; len = 4;
                p[0] = (uint8_t)((s.get(SIG_SET_DRL) ? 0x80 : 0) |
                                 (uint8_t)(nib(s.get(SIG_SET_LAMP_SENS), 7) << 4) |
                                 (uint8_t)(nib(s.get(SIG_SET_ROOM_DELAY), 3) << 2) |
                                 nib(s.get(SIG_SET_LAMP_AUTOOFF), 3));
                p[1] = (uint8_t)nib(s.get(SIG_SET_LOCK1), 255);
                p[2] = (uint8_t)nib(s.get(SIG_SET_LOCK2), 255);
                p[3] = (uint8_t)((uint8_t)(nib(s.get(SIG_SET_GUIDELINE), 2) << 2) |
                                 nib(s.get(SIG_SET_RELOCK), 3));
                return true;
            }
            case M_AMP: {
                if (!(s.has(SIG_AMP_FAD) || s.has(SIG_AMP_BAL) || s.has(SIG_AMP_BASS) ||
                      s.has(SIG_AMP_TREBLE) || s.has(SIG_AMP_MID) || s.has(SIG_AMP_ASL) ||
                      s.has(SIG_AMP_VOLUME) || s.has(SIG_AMP_SDVC) || s.has(SIG_AMP_BEEP)))
                    return false;
                cmd = 0x31; len = 5;
                p[0] = (uint8_t)((nib(s.get(SIG_AMP_FAD), 14) << 4) | nib(s.get(SIG_AMP_BAL), 14));
                p[1] = (uint8_t)((tone(s.get(SIG_AMP_BASS)) << 4) | tone(s.get(SIG_AMP_TREBLE)));
                p[2] = (uint8_t)((tone(s.get(SIG_AMP_MID)) << 4) |
                                 (s.get(SIG_AMP_ASL) ? 0x08 : 0x01));
                p[3] = (uint8_t)nib(s.get(SIG_AMP_VOLUME), 63);
                p[4] = (uint8_t)((s.get(SIG_AMP_SDVC) ? 0x01 : 0) |
                                 (s.get(SIG_AMP_BEEP) ? 0x02 : 0));
                return true;
            }
            case M_SYSTEM: {
                if (!(s.has(SIG_NODE_AMP) || s.has(SIG_NODE_REAR) || s.has(SIG_NODE_360) ||
                      s.has(SIG_NODE_360_CUTIN) || s.has(SIG_AMP_POWER) || s.has(SIG_MUTE)))
                    return false;
                cmd = 0x32; len = 1;
                // b2 (360 camera node present) makes the unit suppress its own
                // radar UI.
                p[0] = (uint8_t)((s.get(SIG_NODE_AMP)       ? 0x01 : 0) |
                                 (s.get(SIG_NODE_REAR)      ? 0x02 : 0) |
                                 (s.get(SIG_NODE_360)       ? 0x04 : 0) |
                                 (s.get(SIG_NODE_360_CUTIN) ? 0x08 : 0) |
                                 (s.get(SIG_AMP_POWER)      ? 0x40 : 0) |
                                 (s.get(SIG_MUTE)           ? 0x80 : 0));
                return true;
            }
            case M_ILLUM: {
                if (!s.has(SIG_ILLUM_DELAY)) return false;
                cmd = 0x50; len = 2;
                p[0] = (uint8_t)nib(s.get(SIG_ILLUM_DELAY), 3);
                p[1] = 0x00;                                 // reserved
                return true;
            }
            case M_VSET: {
                if (!(s.has(SIG_VSET_RADAR_DIST) || s.has(SIG_VSET_FUEL_UNIT) ||
                      s.has(SIG_VSET_THEME))) return false;
                cmd = 0x52; len = 3;
                p[0] = (uint8_t)(s.get(SIG_VSET_RADAR_DIST) ? 0x80 : 0);
                p[1] = (uint8_t)(s.get(SIG_VSET_FUEL_UNIT)  ? 0x80 : 0);
                p[2] = (uint8_t)nib(s.get(SIG_VSET_THEME), 7);
                return true;
            }
            case M_SEATS: {
                if (!(s.has(SIG_SEAT_HEAT_DRV) || s.has(SIG_SEAT_HEAT_PASS) ||
                      s.has(SIG_SEAT_VENT_DRV) || s.has(SIG_SEAT_VENT_PASS))) return false;
                cmd = 0x58; len = 5;
                p[0] = (uint8_t)((nib(s.get(SIG_SEAT_HEAT_DRV), 15) << 4) |
                                 nib(s.get(SIG_SEAT_HEAT_PASS), 15));
                p[1] = 0x00;                                 // passenger vents + fan
                p[2] = (uint8_t)((nib(s.get(SIG_SEAT_VENT_DRV), 15) << 4) |
                                 nib(s.get(SIG_SEAT_VENT_PASS), 15));
                p[3] = 0x00;
                p[4] = 0x00;
                return true;
            }
            case M_HYBRID: {
                // Gated on hybrid_present alone: even a clear-b7 0x1F may be
                // acted on, so a non-hybrid sends nothing unless pinned.
                if (!s.has(SIG_HYBRID_PRESENT)) return false;
                cmd = 0x1F; len = 2;
                p[0] = (uint8_t)((s.get(SIG_HYBRID_PRESENT) ? 0x80 : 0) |
                                 nib(s.get(SIG_HYBRID_BATT), 127));
                p[1] = (uint8_t)nib(s.get(SIG_HYBRID_FLOW), 63);
                return true;
            }
            case M_COOLANT: {
                if (!s.has(SIG_ENG_TEMP)) return false;
                // D1 unused. The tablet shows (78 - D2) mod 256, in °C, on its
                // "oil temp" (owner's sweep 2026-10-25: D2 0 -> 78, 40 -> 38,
                // 78 -> 0, 160 -> 174; docs/hu-decoder.md §2b), not D2 - 40 as
                // read from the decoder. So D2 = 78 - T, wrapping for T > 78;
                // below 0 °C it would show 256 + T, so the floor is 0.
                int32_t t = s.get(SIG_ENG_TEMP);
                if (t < 0) t = 0;
                if (t > 255) t = 255;
                cmd = 0x7D; len = 3;
                p[0] = 0x09; p[1] = 0x00; p[2] = (uint8_t)(78 - t);
                return true;
            }
            case M_GEAR: {
                // D1 bits are other flags (codes 138/139, 1049 idx 12): left 0.
                // D2's low nibble goes to 1049 idx 4, the shape of PRND
                // (docs/hu-decoder.md §2b); the values are the bench's to find.
                if (!(_exp & EXP_GEAR) || !s.has(SIG_PRND)) return false;
                cmd = 0x7D; len = 3;
                p[0] = 0x06; p[1] = 0x00; p[2] = nib(s.get(SIG_PRND), 15);
                return true;
            }
            case M_VERSION: {
                // Liveness: shown in the unit's "Display CANBUS Info" screen.
                cmd = 0x30; len = 16;
                for (uint8_t i = 0; i < 16; ++i)
                    p[i] = _t.version[i] ? (uint8_t)_t.version[i] : (uint8_t)' ';
                return true;
            }
            default: return false;
        }
    }

    bool Rav4Emitter::preview(uint8_t msgIndex, const State& s,
                              uint8_t* frame, uint8_t& frameLen) const
    {
        uint8_t cmd = 0, len = 0, p[MAX_PAYLOAD];
        if (!build(msgIndex, s, cmd, p, len)) return false;
        frameLen = buildFrame(cmd, p, len, frame);
        return frameLen != 0;
    }

    bool Rav4Emitter::sendNow(uint8_t msgIndex, const State& s)
    {
        uint8_t cmd = 0, len = 0, p[MAX_PAYLOAD];
        if (!build(msgIndex, s, cmd, p, len)) return false;
        emit(cmd, p, len);
        if (msgIndex < M_COUNT) _sched[msgIndex].lastMs = 1;   // restart its interval
        return true;
    }

    bool Rav4Emitter::sendByCmd(uint8_t cmd, const State& s)
    {
        for (uint8_t i = 0; i < M_COUNT; ++i) {
            if (kCmd[i] != cmd) continue;
            // 0x28 maps to two rows; send only the selected form.
            if (cmd == 0x28) {
                const uint8_t want = _t.cmd28 ? M_AC : M_TEMP;
                if (i != want) continue;
            }
            if (sendNow(i, s)) return true;
        }
        return false;
    }

    bool Rav4Emitter::acTrusted(const State& s)
    {
        static const uint8_t kSigs[] = {
            SIG_AC_ON, SIG_AC_COMPRESSOR, SIG_AC_RECIRC, SIG_AC_AUTO_HI,
            SIG_AC_AUTO_LO, SIG_AC_DUAL, SIG_AC_MAX_FRONT, SIG_AC_REAR,
            SIG_AC_VENT_UP, SIG_AC_VENT_FACE, SIG_AC_VENT_FLOOR,
            SIG_AC_DISPLAY_REQ, SIG_AC_FAN, SIG_AC_TEMP_DRV,
            SIG_AC_TEMP_PASS, SIG_AC_DEFOG, SIG_AC_AQS, SIG_AC_ECO,
            SIG_AC_UNIT_F,
        };
        for (uint8_t sg : kSigs)
            if (s.pinned(sg) || (s.has(sg) && Vehicle::verified(sg))) return true;
        return false;
    }

    void Rav4Emitter::tick(const State& s, uint32_t nowMs)
    {
        if (!_enabled) return;
        // The outside temperature shown in 0x28, held against flicker.
        if (s.has(SIG_OUT_TEMP) && !s.pinned(SIG_OUT_TEMP)) _temp.step(s.get(SIG_OUT_TEMP), nowMs);

        for (uint8_t i = 0; i < M_COUNT; ++i) {
            if (!_msgOn[i]) continue;
            // The two 0x28 forms are mutually exclusive on the wire.
            if (i == M_TEMP && _t.cmd28 != 0) continue;
            if (i == M_AC   && _t.cmd28 == 0) continue;
            if (i == M_COOLANT && !(_exp & EXP_COOLANT)) continue;

            uint8_t cmd = 0, len = 0, p[MAX_PAYLOAD];
            if (!build(i, s, cmd, p, len)) continue;

            // On-change compares a hash of the whole payload (FNV-1a over len +
            // bytes); 0xFFFFFFFF is Sched's "never sent", so it is avoided.
            uint32_t sig = 2166136261u;
            sig = (sig ^ len) * 16777619u;
            for (uint8_t b = 0; b < len; ++b) sig = (sig ^ p[b]) * 16777619u;
            if (sig == 0xFFFFFFFF) sig = 0xFFFFFFFE;

            if (due(_sched[i], nowMs, kInterval[i], sig, kOnChange[i]))
                emit(cmd, p, len);
        }
    }
}
