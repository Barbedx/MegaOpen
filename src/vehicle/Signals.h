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
        GRP_ENGINE,      // battery, oil, pedals, gearbox
        GRP_SAFETY,      // belts, airbag, locks, roof
        GRP_DYNAMICS,    // accelerations, yaw
        GRP_USER,        // the owner's own (formulas)
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

        // ---- Mégane II broadcast (X84 matrix), 2026-10 -----------------------
        // APPENDED, never inserted: the NVS decode blob stores signals by
        // index. Positions are in decode_defaults.inc; every one is a
        // hypothesis until the car confirms it. Most have no Raise field and
        // are for the web page and the panel alerts only.
        SIG_FUEL_LEVEL,        // litres, the cluster's (0x5C5)
        SIG_FUEL_LOW,          // the cluster's low-fuel warning
        SIG_FUEL_GAUGE_DIAG,   // 0 ok, 1 open circuit, 2 short
        SIG_ODO_CLUSTER,       // km, the cluster's own (0x5C5); `odometer` is 0x5FD
        SIG_FUEL_LEVEL_APV,    // litres, the after-sales backup copy (0x715)
        SIG_OIL_LEVEL,         // the level the cluster shows, 0-15
        SIG_MIL,               // check-engine lamp state, 0-3
        SIG_SERVICE_KM,        // km to the next service
        SIG_FUEL_COUNTER,      // RAW wrapping counter, 80 mm³ a count (0x551)
        SIG_BATT_V,            // V x100
        SIG_OIL_PRESS_SW,      // oil-pressure switch
        SIG_BATT_LAMP,         // charge warning lamp
        SIG_AC_COMP_UPC,       // A/C compressor as the UPC reports it; not
                               // ac_compressor, which would start the 0x28 A/C form
        SIG_REAR_DEFROST,      // heated rear window on
        SIG_THROTTLE,          // accelerator pedal, % x10
        SIG_CLUTCH,            // clutch switch
        SIG_NEUTRAL,           // gearbox in neutral
        SIG_REVERSE_UPC,       // reverse, the UPC's copy (0x215)
        SIG_LOCKED,            // raw 0-3: bit0 boot locked, bit1 doors locked
        SIG_BELT_DRV,          // 1 = fastened (the matrix's 00, DF_STATE2_INV)
        SIG_BELT_PASS,         // 1 = fastened
        SIG_PASS_PRESENT,      // 1 = someone on the passenger seat (matrix 00)
        SIG_AIRBAG_FAULT,
        SIG_PASS_AIRBAG_OFF,
        SIG_ROOF_POS,          // the CC's roof, 0-7
        SIG_ROOF_FAULT,
        SIG_CLIM_AIR_SET,      // blown-air setpoint, °C (not the user's setting)
        SIG_CLIM_FAN_STATE,    // 0-3
        SIG_CLIM_AC_SEL,       // A/C selected on the climate panel
        SIG_CLIM_EVAP,         // evaporator, °C x10
        SIG_AC_REQUEST,        // the UCH's A/C request
        SIG_REAR_DEFROST_REQ,  // 0-3
        SIG_WIPERS,            // front wiper request, 0-7
        SIG_ROLLING,           // the UCH's "vehicle moving"
        SIG_SUN_LUX,           // sunlight, lux
        SIG_ACCEL_LONG,        // m/s² x100, signed
        SIG_ACCEL_LAT,         // g x1000, signed
        SIG_YAW_RATE,          // °/s x10, signed

        // ---- derived on the car task (Derived.h), never decoded ------------------
        // New names, not range/fuel_inst/fuel_avg: those feed Raise messages,
        // and what goes to the head unit stays a toggle of its own.
        SIG_GEAR,              // -1 R, 0 N or unknown, 1..6
        SIG_GEAR_RATIO,        // km/h per 1000 rpm x10: what the gear table is learnt from
        SIG_FUEL_RATE,         // L/h x10
        SIG_CONS_INST,         // L/100km x10, while rolling
        SIG_CONS_AVG,          // L/100km x10, since boot
        SIG_RANGE_CALC,        // km, fuel_level over cons_avg (a factory formula, Formulas.h)
        SIG_TRIP_FUEL,         // L x100, since boot
        SIG_TRIP_KM,           // km x10, since boot
        // A head-unit probe, not a car value: 0x7D 06 D2's low nibble, set by
        // hand from the map page to find PRND (its field is off by default).
        SIG_PRND,
        // 0x24 D1 b7 on the head unit (docs/hu-decoder.md §2a). The X84
        // broadcast matrix has no bonnet contact, so no factory row: bind it
        // by hand if a car has one. Last, so the saved map's indices hold.
        SIG_BONNET,
        // 0x061 (ECM_to_GCU) b7 - 40: the intake air, per the X84 broadcast matrix
        // (v84.05). Last, like SIG_BONNET, so the saved map's indices hold.
        SIG_INTAKE_TEMP,
        // From the climate ECU's `21 C1` (src/app/CarDiag.h), not a broadcast
        // frame: the cabin, °C x10, and the sun sensor, W/m².
        SIG_CABIN_TEMP,
        SIG_CLIM_SUN,
        // Safety, derived (Vehicle::Derived): moving at 10 km/h or more with neither
        // the dipped nor the main beam on; moving at 5 km/h or more with the
        // handbrake up. Yes/no, for the panel alerts.
        SIG_DRIVE_NO_LIGHTS,
        SIG_DRIVE_HANDBRAKE,
        // The tank in per cent x10: the factory formula fuel_level * 100 / 60
        // (Formulas.h; the Mégane II's tank is 60 L).
        SIG_FUEL_PCT,
        // km to the service to the km (Vehicle::Derived): service_km is 0x715's
        // step of 150 km; this one counts the km down from the step's edge.
        SIG_SERVICE_CALC,

        // The owner's own signals (docs/design/rules.md, slice 3): slots named
        // at run time by their formulas (setUserSignal). An unnamed slot does
        // not exist: its name is "" and no list shows it. Last: everything
        // saved names a signal by NAME, so their ids may move.
        SIG_USER_0,
        SIG_USER_LAST = SIG_USER_0 + 63,

        SIG_COUNT
    };
    constexpr uint8_t USER_SIGNALS = SIG_USER_LAST - SIG_USER_0 + 1;
    static_assert(SIG_COUNT <= 255, "a signal id is a byte, and 255 is no signal");
    constexpr bool isUser(uint8_t s) { return s >= SIG_USER_0 && s < SIG_COUNT; }

    // "No signal" as it is SAVED (the alert blob, the car screen's NVS keys).
    // In RAM an empty row is SIG_COUNT, but SIG_COUNT moves each time a
    // signal is appended, so a saved SIG_COUNT read by a newer build is that
    // build's new signal: 1.6.3's empty alert rows and car-screen slots came
    // back as fuel_pct. Saved, an empty row is SIG_SAVED_NONE.
    constexpr uint8_t SIG_SAVED_NONE = 0xFF;
    // What builds up to 1.6.3 saved for an empty row: their SIG_COUNT, which
    // was SIG_FUEL_PCT (up to 1.6.2) or SIG_FUEL_PCT + 1 (1.6.3). An older
    // record's value at or past this is read as empty (a fuel_pct chosen on
    // 1.6.3 alone is lost with it).
    constexpr uint8_t SIG_LEGACY_EMPTY_FROM = SIG_FUEL_PCT;
    // A saved id to RAM's: empty -> SIG_COUNT. `legacy`: a record from before
    // SIG_SAVED_NONE.
    constexpr uint8_t sigFromSaved(uint8_t v, bool legacy)
    {
        return legacy ? (v >= SIG_LEGACY_EMPTY_FROM ? (uint8_t)SIG_COUNT : v)
                      : (v < SIG_COUNT ? v : (uint8_t)SIG_COUNT);
    }
    constexpr uint8_t sigToSaved(uint8_t s) { return s < SIG_COUNT ? s : SIG_SAVED_NONE; }

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
    // A built-in signal, or a user slot with a name.
    bool        exists(uint8_t s);

    // A user signal's description; null name = the slot is free.
    struct UserDef {
        const char* name    = nullptr;   // [a-z_][a-z0-9_]*, at most USER_NAME chars
        const char* unit    = "";        // at most USER_UNIT chars
        uint8_t     decimals = 0;
        bool        boolean = false;
    };
    constexpr uint8_t USER_NAME = 23, USER_UNIT = 11;
    // A whole set of user slots at once: slot i of `defs` (USER_SIGNALS of
    // them). Readers on any task see the old set or the new one, never half
    // of one (two tables, the live one swapped). One writer: the car task.
    void setUserSignals(const UserDef* defs);

    // Is this signal's value trustworthy: bound to CAN by rows that are all
    // measured (no DF_HYPOTHESIS), and not pinned. The car task owns the
    // answer and refreshes it whenever the map or a pin changes
    // (setVerified); anyone may read it, from any task. False until then.
    bool verified(uint8_t s);
    void setVerified(uint8_t s, bool v);
}
