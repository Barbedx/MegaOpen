#pragma once
#include <stdint.h>

// Vehicle signal vocabulary. Each signal comes from decoded CAN or a value pinned
// from the web UI (ARCHITECTURE.md §7.3). The web page builds itself from the
// metadata table, so a new signal needs no HTML change.
//
// Covers the whole RAV4/Toyota half of the Raise protocol (docs/raise-protocol.md
// §2a); signals with no Mégane source (TPMS, radar) stay silent until pinned.
//
// Raw-byte signals: A/C setpoints and TPMS pressures are the literal wire byte,
// because their encodings are CONFLICT in the protocol notes. /canbox offers
// °C / bar helpers.
namespace Vehicle
{
    // Web page sections only.
    enum Group : uint8_t {
        GRP_DRIVE = 0,   // speed, rpm, steering, odometer
        GRP_BODY,        // doors, ignition, brakes, temperature
        GRP_LIGHTS,      // exterior lighting + indicators
        GRP_TRIP,        // trip computer and fuel
        GRP_CLIMATE,     // 0x28 A/C form + 0x58 seats
        GRP_RADAR,       // 0x1D / 0x1E parking sensors
        GRP_TPMS,        // 0x25
        GRP_AUDIO,       // 0x31 amplifier + 0x32 node presence
        GRP_SETTINGS,    // 0x26 / 0x50 / 0x52 vehicle settings
        GRP_COUNT
    };

    enum Sig : uint8_t {
        // ---- drive ---------------------------------------------------------
        SIG_SPEED = 0,     // km/h x100
        SIG_RPM,           // rpm
        SIG_STEERING,      // degrees x10, signed (- = left)
        SIG_ODOMETER,      // km

        // ---- body ----------------------------------------------------------
        SIG_OUT_TEMP,      // °C
        SIG_ENG_TEMP,      // °C
        SIG_DOOR_FL,
        SIG_DOOR_FR,
        SIG_DOOR_RL,
        SIG_DOOR_RR,
        SIG_BOOT,
        SIG_REVERSE,
        SIG_HANDBRAKE,
        SIG_BRAKE,
        SIG_KEY_ON,
        SIG_KEY_ACC,
        SIG_BACKLIGHT,     // instrument illumination 0x00-0xFE (0xFF = off)

        // ---- lights ---------------------------------------------------------
        SIG_LIGHT_POS,
        SIG_LIGHT_DIPPED,
        SIG_LIGHT_MAIN,
        SIG_IND_LEFT,
        SIG_IND_RIGHT,
        SIG_FOG_FRONT,
        SIG_FOG_REAR,

        // ---- trip computer ---------------------------------------------------
        // Pin-only until a CAN decode exists.
        SIG_AVG_SPEED,     // km/h x100 — the trailing field of 0x7D 03
        SIG_TRIP1,         // 0.1 km
        SIG_TRIP2,         // 0.1 km
        SIG_TRIP_ELAPSED,  // MINUTES (not seconds — see docs/raise-protocol.md)
        SIG_RANGE,         // cruising range, km
        SIG_FUEL_INST,     // instantaneous consumption, L/100km x10
        SIG_FUEL_AVG,      // current-trip average consumption, L/100km x10

        // ---- climate: 0x28 in its A/C form, D0/D1/D4 ------------------------
        SIG_AC_ON,
        SIG_AC_COMPRESSOR,
        SIG_AC_RECIRC,
        SIG_AC_AUTO_HI,
        SIG_AC_AUTO_LO,
        SIG_AC_DUAL,
        SIG_AC_MAX_FRONT,
        SIG_AC_REAR,
        SIG_AC_VENT_UP,        // D1 b7 windscreen/up vent
        SIG_AC_VENT_FACE,      // D1 b6
        SIG_AC_VENT_FLOOR,     // D1 b5
        SIG_AC_DISPLAY_REQ,    // D1 b4 — asks the HU to pop the climate overlay up
        SIG_AC_FAN,            // D1 b3-0, 0-7
        SIG_AC_TEMP_DRV,       // D2 RAW WIRE BYTE (0x00 LO, 0x1F HI, 0x20-0x23 = 16-19 °C)
        SIG_AC_TEMP_PASS,      // D3 RAW WIRE BYTE, same encoding
        SIG_AC_DEFOG,          // D4
        SIG_AC_AQS,            // D4
        SIG_AC_ECO,            // D4
        SIG_AC_UNIT_F,         // D4 b0: 0 = °C, 1 = °F
        // 0x58 seat heat/vent. Crown-gated in the HU; may be inert on a RAV4 profile.
        SIG_SEAT_HEAT_DRV,     // 0-15
        SIG_SEAT_HEAT_PASS,
        SIG_SEAT_VENT_DRV,
        SIG_SEAT_VENT_PASS,

        // ---- parking radar: 0x1E rear, 0x1D front ---------------------------
        // Distance: 0 = no display, 1 = nearest ... 4 = farthest. One reference
        // implementation emits the inverse (bars fill backing away = that conflict).
        SIG_RADAR_RL,
        SIG_RADAR_RLM,
        SIG_RADAR_RRM,
        SIG_RADAR_RR,
        SIG_RADAR_FR,          // 0x1D is ordered RIGHT TO LEFT, mirrored vs 0x1E
        SIG_RADAR_FRM,
        SIG_RADAR_FLM,
        SIG_RADAR_FL,
        SIG_RADAR_ENABLE,      // 0x1E D4 b5 — the head unit DISCARDS every 0x1D
                               // while this is clear, so 0x1E has to go first
        SIG_RADAR_DISPLAY,     // 0x1E D4 b7
        SIG_RADAR_FAR,         // 0x1E D4 b6, near/far threshold
        SIG_RADAR_VOLUME,      // 0x1E D4 b2-0, 1-5

        // ---- TPMS: 0x25 -----------------------------------------------------
        SIG_TPMS_PRESENT,      // b7 — gates the whole panel
        SIG_TPMS_ALARM,
        SIG_TPMS_SHOW_SPARE,
        SIG_TPMS_DIAGRAM,      // b2: 0 = row layout, 1 = car diagram
        SIG_TPMS_UNIT,         // b1-0: 0 = 0.1 bar, 1 = PSI, 2 = 2.5 kPa
        SIG_TPMS_FL,           // RAW WIRE BYTE, 0xFF = no data
        SIG_TPMS_FR,
        SIG_TPMS_RL,
        SIG_TPMS_RR,
        SIG_TPMS_SPARE,

        // ---- audio: 0x31 amplifier, 0x32 node presence ----------------------
        SIG_AMP_FAD,           // 0-14 (0x00-0x0E)
        SIG_AMP_BAL,           // 0-14
        SIG_AMP_BASS,          // -5..+5, sent with the +7 offset
        SIG_AMP_TREBLE,        // -5..+5
        SIG_AMP_MID,           // -5..+5
        SIG_AMP_ASL,           // 0 off / 1 on
        SIG_AMP_VOLUME,        // 0-63 — also what the HU tells US on 0x84 idx 0x07
        SIG_AMP_SDVC,          // speed-dependent volume
        SIG_AMP_BEEP,
        SIG_NODE_AMP,          // 0x32 b0
        SIG_NODE_REAR,         // b1
        SIG_NODE_360,          // b2 — set, and the HU hides its own radar UI
        SIG_NODE_360_CUTIN,    // b3
        SIG_AMP_POWER,         // b6
        SIG_MUTE,              // b7

        // ---- vehicle settings: 0x26, 0x50, 0x52 -----------------------------
        SIG_SET_DRL,           // 0x26 D0 b7
        SIG_SET_LAMP_SENS,     // 0x26 D0 b6-4, 0-7
        SIG_SET_ROOM_DELAY,    // 0x26 D0 b3-2, 0-3
        SIG_SET_LAMP_AUTOOFF,  // 0x26 D0 b1-0, 0-3
        SIG_SET_LOCK1,         // 0x26 D1 RAW: door-lock settings block
        SIG_SET_LOCK2,         // 0x26 D2 RAW
        SIG_SET_GUIDELINE,     // 0x26 D3 b3-2: 0 dynamic, 1 static, 2 warning lines
        SIG_SET_RELOCK,        // 0x26 D3 b1-0
        SIG_ILLUM_DELAY,       // 0x50 D0 b1-0: 0 off, 1 = 7.5 s, 2 = 15 s, 3 = 30 s
        SIG_VSET_RADAR_DIST,   // 0x52 D0 b7
        SIG_VSET_FUEL_UNIT,    // 0x52 D1 b7: 0 = L/100km, 1 = km/L
        SIG_VSET_THEME,        // 0x52 D2 b2-0

        // ---- hybrid: 0x1F ----------------------------------------------------
        // Layout from RAV4_BODY_ENGINE_2018_2020.pdf §8. Silent unless
        // hybrid_present is pinned (a Mégane is not a hybrid).
        SIG_HYBRID_PRESENT,    // D0 b7 — gates the whole panel
        SIG_HYBRID_BATT,       // D0 b6-0 — charge level, 8 bars on the original
        SIG_HYBRID_FLOW,       // D1 RAW bitmask: b0 motor->batt, b1 motor->wheels,
                               // b2 engine->motor, b3 engine->wheels,
                               // b4 batt->motor, b5 regen

        SIG_COUNT
    };

    struct Meta {
        const char* name;
        const char* unit;
        int32_t     min, max;
        uint8_t     decimals;   // display only: value is always a scaled integer
        bool        boolean;
        uint8_t     group;
    };

    const Meta& meta(uint8_t s);
    uint8_t     sigByName(const char* name);   // SIG_COUNT when unknown
}
