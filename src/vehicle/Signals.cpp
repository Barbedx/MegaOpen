#include "Signals.h"
#include <string.h>

namespace Vehicle
{
    // Order must match enum Sig; indexed, not searched.
    static const Meta kMeta[SIG_COUNT] = {
        // ---- drive ----------------------------------------------------------
        // Wire limit, not a car's: 0x7D03 carries km/h x100 as u16. Pinning must
        // be able to send any value the protocol can carry.
        {"speed",        "km/h",    0,   65535, 2, false, GRP_DRIVE},
        {"rpm",          "rpm",     0,    8000, 0, false, GRP_DRIVE},
        {"steering",     "deg",  -5400,   5400, 1, false, GRP_DRIVE},
        {"odometer",     "km",      0,  999999, 0, false, GRP_DRIVE},

        // ---- body -----------------------------------------------------------
        {"out_temp",     "C",     -40,      87, 0, false, GRP_BODY},
        {"eng_temp",     "C",     -40,     215, 0, false, GRP_BODY},
        {"door_fl",      "",        0,       1, 0, true,  GRP_BODY},
        {"door_fr",      "",        0,       1, 0, true,  GRP_BODY},
        {"door_rl",      "",        0,       1, 0, true,  GRP_BODY},
        {"door_rr",      "",        0,       1, 0, true,  GRP_BODY},
        {"boot",         "",        0,       1, 0, true,  GRP_BODY},
        {"reverse",      "",        0,       1, 0, true,  GRP_BODY},
        {"handbrake",    "",        0,       1, 0, true,  GRP_BODY},
        {"brake",        "",        0,       1, 0, true,  GRP_BODY},
        {"key_on",       "",        0,       1, 0, true,  GRP_BODY},
        {"key_acc",      "",        0,       1, 0, true,  GRP_BODY},
        {"backlight",    "",        0,     255, 0, false, GRP_BODY},

        // ---- lights ----------------------------------------------------------
        {"light_pos",    "",        0,       1, 0, true,  GRP_LIGHTS},
        {"light_dip",    "",        0,       1, 0, true,  GRP_LIGHTS},
        {"light_main",   "",        0,       1, 0, true,  GRP_LIGHTS},
        {"ind_left",     "",        0,       1, 0, true,  GRP_LIGHTS},
        {"ind_right",    "",        0,       1, 0, true,  GRP_LIGHTS},
        {"fog_front",    "",        0,       1, 0, true,  GRP_LIGHTS},
        {"fog_rear",     "",        0,       1, 0, true,  GRP_LIGHTS},

        // ---- trip -------------------------------------------------------------
        // Same 0x7D03 field width as `speed`.
        {"avg_speed",    "km/h",    0,   65535, 2, false, GRP_TRIP},
        {"trip1",        "km",      0,   99999, 1, false, GRP_TRIP},
        {"trip2",        "km",      0,   99999, 1, false, GRP_TRIP},
        {"trip_min",     "min",     0,   65535, 0, false, GRP_TRIP},
        {"range",        "km",      0,    9999, 0, false, GRP_TRIP},
        {"fuel_inst",    "L/100km", 0,    9999, 1, false, GRP_TRIP},
        {"fuel_avg",     "L/100km", 0,    9999, 1, false, GRP_TRIP},

        // ---- climate ----------------------------------------------------------
        {"ac_on",         "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_compressor", "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_recirc",     "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_auto_hi",    "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_auto_lo",    "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_dual",       "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_max_front",  "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_rear",       "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_vent_up",    "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_vent_face",  "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_vent_floor", "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_display_req","",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_fan",        "0-7",    0,       7, 0, false, GRP_CLIMATE},
        {"ac_temp_drv",   "raw",    0,     255, 0, false, GRP_CLIMATE},
        {"ac_temp_pass",  "raw",    0,     255, 0, false, GRP_CLIMATE},
        {"ac_defog",      "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_aqs",        "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_eco",        "",       0,       1, 0, true,  GRP_CLIMATE},
        {"ac_unit_f",     "",       0,       1, 0, true,  GRP_CLIMATE},
        {"seat_heat_drv", "0-15",   0,      15, 0, false, GRP_CLIMATE},
        {"seat_heat_pass","0-15",   0,      15, 0, false, GRP_CLIMATE},
        {"seat_vent_drv", "0-15",   0,      15, 0, false, GRP_CLIMATE},
        {"seat_vent_pass","0-15",   0,      15, 0, false, GRP_CLIMATE},

        // ---- radar --------------------------------------------------------------
        {"radar_rl",      "0-4",    0,       4, 0, false, GRP_RADAR},
        {"radar_rlm",     "0-4",    0,       4, 0, false, GRP_RADAR},
        {"radar_rrm",     "0-4",    0,       4, 0, false, GRP_RADAR},
        {"radar_rr",      "0-4",    0,       4, 0, false, GRP_RADAR},
        {"radar_fr",      "0-4",    0,       4, 0, false, GRP_RADAR},
        {"radar_frm",     "0-4",    0,       4, 0, false, GRP_RADAR},
        {"radar_flm",     "0-4",    0,       4, 0, false, GRP_RADAR},
        {"radar_fl",      "0-4",    0,       4, 0, false, GRP_RADAR},
        {"radar_enable",  "",       0,       1, 0, true,  GRP_RADAR},
        {"radar_display", "",       0,       1, 0, true,  GRP_RADAR},
        {"radar_far",     "",       0,       1, 0, true,  GRP_RADAR},
        {"radar_volume",  "1-5",    0,       5, 0, false, GRP_RADAR},

        // ---- tpms ----------------------------------------------------------------
        {"tpms_present",  "",       0,       1, 0, true,  GRP_TPMS},
        {"tpms_alarm",    "",       0,       1, 0, true,  GRP_TPMS},
        {"tpms_spare_on", "",       0,       1, 0, true,  GRP_TPMS},
        {"tpms_diagram",  "",       0,       1, 0, true,  GRP_TPMS},
        {"tpms_unit",     "0-2",    0,       2, 0, false, GRP_TPMS},
        {"tpms_fl",       "raw",    0,     255, 0, false, GRP_TPMS},
        {"tpms_fr",       "raw",    0,     255, 0, false, GRP_TPMS},
        {"tpms_rl",       "raw",    0,     255, 0, false, GRP_TPMS},
        {"tpms_rr",       "raw",    0,     255, 0, false, GRP_TPMS},
        {"tpms_spare",    "raw",    0,     255, 0, false, GRP_TPMS},

        // ---- audio ----------------------------------------------------------------
        {"amp_fad",       "0-14",   0,      14, 0, false, GRP_AUDIO},
        {"amp_bal",       "0-14",   0,      14, 0, false, GRP_AUDIO},
        {"amp_bass",      "-5..5",  -5,      5, 0, false, GRP_AUDIO},
        {"amp_treble",    "-5..5",  -5,      5, 0, false, GRP_AUDIO},
        {"amp_mid",       "-5..5",  -5,      5, 0, false, GRP_AUDIO},
        {"amp_asl",       "",       0,       1, 0, true,  GRP_AUDIO},
        {"amp_volume",    "0-63",   0,      63, 0, false, GRP_AUDIO},
        {"amp_sdvc",      "",       0,       1, 0, true,  GRP_AUDIO},
        {"amp_beep",      "",       0,       1, 0, true,  GRP_AUDIO},
        {"node_amp",      "",       0,       1, 0, true,  GRP_AUDIO},
        {"node_rear",     "",       0,       1, 0, true,  GRP_AUDIO},
        {"node_360",      "",       0,       1, 0, true,  GRP_AUDIO},
        {"node_360_cutin","",       0,       1, 0, true,  GRP_AUDIO},
        {"amp_power",     "",       0,       1, 0, true,  GRP_AUDIO},
        {"mute",          "",       0,       1, 0, true,  GRP_AUDIO},

        // ---- settings ---------------------------------------------------------------
        {"set_drl",         "",     0,       1, 0, true,  GRP_SETTINGS},
        {"set_lamp_sens",   "0-7",  0,       7, 0, false, GRP_SETTINGS},
        {"set_room_delay",  "0-3",  0,       3, 0, false, GRP_SETTINGS},
        {"set_lamp_autooff","0-3",  0,       3, 0, false, GRP_SETTINGS},
        {"set_lock1",       "raw",  0,     255, 0, false, GRP_SETTINGS},
        {"set_lock2",       "raw",  0,     255, 0, false, GRP_SETTINGS},
        {"set_guideline",   "0-2",  0,       2, 0, false, GRP_SETTINGS},
        {"set_relock",      "0-3",  0,       3, 0, false, GRP_SETTINGS},
        {"illum_delay",     "0-3",  0,       3, 0, false, GRP_SETTINGS},
        {"vset_radar_dist", "",     0,       1, 0, true,  GRP_SETTINGS},
        {"vset_fuel_unit",  "",     0,       1, 0, true,  GRP_SETTINGS},
        {"vset_theme",      "0-7",  0,       7, 0, false, GRP_SETTINGS},

        // ---- hybrid -------------------------------------------------------------------
        {"hybrid_present",  "",     0,       1, 0, true,  GRP_DRIVE},
        {"hybrid_batt",     "0-8",  0,     8,   0, false, GRP_DRIVE},
        {"hybrid_flow",     "raw",  0,      63, 0, false, GRP_DRIVE},

        // ---- Mégane II broadcast: ranges are what the wire field can carry ------------
        {"fuel_level",      "L",    0,     127, 0, false, GRP_TRIP},
        {"fuel_low",        "",     0,       1, 0, true,  GRP_TRIP},
        {"fuel_gauge_diag", "0-3",  0,       3, 0, false, GRP_TRIP},
        {"odo_cluster",     "km",   0, 16777215, 0, false, GRP_DRIVE},
        {"fuel_level_apv",  "L",    0,     127, 0, false, GRP_TRIP},
        {"oil_level",       "0-15", 0,      15, 0, false, GRP_ENGINE},
        {"mil",             "0-3",  0,       3, 0, false, GRP_ENGINE},
        {"service_km",      "km",   0,   63750, 0, false, GRP_ENGINE},
        {"fuel_counter",    "raw",  0,     255, 0, false, GRP_ENGINE},
        {"batt_v",          "V",    0,    1600, 2, false, GRP_ENGINE},
        {"oil_press_sw",    "",     0,       1, 0, true,  GRP_ENGINE},
        {"batt_lamp",       "",     0,       1, 0, true,  GRP_ENGINE},
        {"ac_comp_upc",     "",     0,       1, 0, true,  GRP_CLIMATE},
        {"rear_defrost",    "",     0,       1, 0, true,  GRP_BODY},
        {"throttle",        "%",    0,    1000, 1, false, GRP_ENGINE},
        {"clutch",          "",     0,       1, 0, true,  GRP_ENGINE},
        {"neutral",         "",     0,       1, 0, true,  GRP_ENGINE},
        {"reverse_upc",     "",     0,       1, 0, true,  GRP_ENGINE},
        {"locked",          "0-3",  0,       3, 0, false, GRP_SAFETY},
        {"belt_drv",        "",     0,       1, 0, true,  GRP_SAFETY},
        {"belt_pass",       "",     0,       1, 0, true,  GRP_SAFETY},
        {"pass_present",    "",     0,       1, 0, true,  GRP_SAFETY},
        {"airbag_fault",    "",     0,       1, 0, true,  GRP_SAFETY},
        {"pass_airbag_off", "",     0,       1, 0, true,  GRP_SAFETY},
        {"roof_pos",        "0-7",  0,       7, 0, false, GRP_SAFETY},
        {"roof_fault",      "",     0,       1, 0, true,  GRP_SAFETY},
        {"clim_air_set",    "C",  -40,     215, 0, false, GRP_CLIMATE},
        {"clim_fan_state",  "0-3",  0,       3, 0, false, GRP_CLIMATE},
        {"clim_ac_sel",     "",     0,       1, 0, true,  GRP_CLIMATE},
        {"clim_evap",       "C",  -400,    875, 1, false, GRP_CLIMATE},
        {"ac_request",      "",     0,       1, 0, true,  GRP_CLIMATE},
        {"rear_defrost_req","0-3",  0,       3, 0, false, GRP_BODY},
        {"wipers",          "0-7",  0,       7, 0, false, GRP_BODY},
        {"rolling",         "",     0,       1, 0, true,  GRP_DYNAMICS},
        {"sun_lux",         "lux",  0,  255000, 0, false, GRP_BODY},
        {"accel_long",      "m/s2", -1000,  275, 2, false, GRP_DYNAMICS},
        {"accel_lat",       "g", -131068, 131072, 3, false, GRP_DYNAMICS},
        {"yaw_rate",        "deg/s", -65534, 65536, 1, false, GRP_DYNAMICS},

        // ---- derived ------------------------------------------------------------------
        {"gear",            "",    -1,       6, 0, false, GRP_ENGINE},
        {"gear_ratio",      "km/h/krpm", 0, 9999, 1, false, GRP_ENGINE},
        {"fuel_rate",       "L/h",  0,    9999, 1, false, GRP_TRIP},
        {"cons_inst",       "L/100km", 0, 9999, 1, false, GRP_TRIP},
        {"cons_avg",        "L/100km", 0, 9999, 1, false, GRP_TRIP},
        {"range_calc",      "km",   0,    9999, 0, false, GRP_TRIP},
        {"trip_fuel",       "L",    0, 9999999, 2, false, GRP_TRIP},
        {"trip_km",         "km",   0, 9999999, 1, false, GRP_TRIP},
        {"prnd",            "0-15", 0,      15, 0, false, GRP_DRIVE},
        {"bonnet",          "",     0,       1, 0, true,  GRP_BODY},
    };

    static_assert(sizeof(kMeta) / sizeof(kMeta[0]) == SIG_COUNT,
                  "Signals.cpp metadata table is out of step with enum Sig — the table is "
                  "INDEXED by the enum, so a missing row shifts every signal after it and "
                  "the web UI silently edits the wrong one.");

    static const Meta kUnknown = {"?", "", 0, 0, 0, false, GRP_COUNT};

    const Meta& meta(uint8_t s) { return s < SIG_COUNT ? kMeta[s] : kUnknown; }

    // One bit a signal, written by the car task only; a byte store is atomic,
    // so a reader sees each signal's old or new answer, never a torn one.
    static volatile uint8_t s_verified[(SIG_COUNT + 7) / 8];

    bool verified(uint8_t s)
    {
        return s < SIG_COUNT && (s_verified[s >> 3] & (1u << (s & 7)));
    }

    void setVerified(uint8_t s, bool v)
    {
        if (s >= SIG_COUNT) return;
        const uint8_t bit = (uint8_t)(1u << (s & 7));
        s_verified[s >> 3] = v ? (uint8_t)(s_verified[s >> 3] | bit) : (uint8_t)(s_verified[s >> 3] & ~bit);
    }

    uint8_t sigByName(const char* name)
    {
        if (!name) return SIG_COUNT;
        for (uint8_t i = 0; i < SIG_COUNT; ++i)
            if (strcmp(kMeta[i].name, name) == 0) return i;
        return SIG_COUNT;
    }
}
