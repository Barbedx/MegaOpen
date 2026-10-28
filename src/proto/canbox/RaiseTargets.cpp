#include "RaiseTargets.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace Canbox
{
    using namespace Vehicle;
    using M = Rav4Emitter;

    namespace
    {
        // The page's sections, in the order they first appear.
        // Ids: the page has the words (web/i18n "k.rgroup.<id>").
        const char* const kGroups[] = {"doors", "drive", "trip", "fuel", "engine", "climate", "lights", "radar", "tyres", "audio", "settings", "hybrid"};

        enum : uint8_t { G_DOORS, G_DRIVE, G_TRIP, G_FUEL, G_ENGINE, G_CLIMATE, G_LIGHTS, G_RADAR, G_TYRES, G_AUDIO, G_SETTINGS, G_HYBRID };

        // The Raise command each message goes out as (Rav4Emitter::Msg order).
        const char* const kMsgCmd[] = {
            "24", "28", "28 A/C", "29", "7D 01", "7D 03", "7D 04", "7D 0A", "21", "22", "23",
            "1E", "1D", "25", "26", "31", "32", "50", "52", "58", "1F", "30", "7D 09",
            "7D 06",
        };
        static_assert(sizeof(kMsgCmd) / sizeof(kMsgCmd[0]) == M::MSG_COUNT, "kMsgCmd is out of step with Msg");

        // A car value the owner looks for on the tablet that protocol 112
        // has no field for: the page shows the value and says why it does not
        // go out (the command column is empty).
        enum : uint8_t { NF_BATT = 0, NF_NONE = 0xFF };
        // "": the words are the page's (k.raise.<id>.why); non-null says "not sent".
        const char* const kNoFw[] = {
            // 0x41 02 is VW's; this head unit decodes 0x41 into Toyota codes
            // and 0x7D 0B not at all (docs/hu-decoder.md §2b-2c). Another
            // field can carry it: «Звідки» → batt_v ÷10 on range, fuel_avg
            // or fuel_pct.
            "",
        };

        // 8 B a row: the name, the signal, the message, the section, and why
        // it is not sent.
        struct Row {
            const char* name;
            uint8_t     sig;
            uint8_t     msg;
            uint8_t     group;
            uint8_t     nofw;
        };

        // A signal the emitter sends; a car value it has no field for.
        #define T(sig, msg, g, name)   {name, sig, M::msg, g, NF_NONE}
        #define X(sig, g, name, nf)    {name, sig, NO_MSG, g, nf}

        const Row kRows[] = {
            T(SIG_DOOR_FL, MSG_DOORS, G_DOORS, nullptr),
            T(SIG_DOOR_FR, MSG_DOORS, G_DOORS, nullptr),
            T(SIG_DOOR_RL, MSG_DOORS, G_DOORS, nullptr),
            T(SIG_DOOR_RR, MSG_DOORS, G_DOORS, nullptr),
            T(SIG_BOOT, MSG_DOORS, G_DOORS, nullptr),
            T(SIG_KEY_ON, MSG_DOORS, G_DOORS, nullptr),
            T(SIG_BONNET, MSG_DOORS, G_DOORS, nullptr),

            T(SIG_SPEED, MSG_SPEED, G_DRIVE, nullptr),
            T(SIG_AVG_SPEED, MSG_TRIP, G_DRIVE, nullptr),   // also 7D 03, which needs speed
            T(SIG_RPM, MSG_RPM, G_DRIVE, nullptr),
            T(SIG_STEERING, MSG_STEER, G_DRIVE, nullptr),
            T(SIG_PRND, MSG_GEAR, G_DRIVE, nullptr),

            T(SIG_ODOMETER, MSG_ODO, G_TRIP, nullptr),
            T(SIG_TRIP1, MSG_ODO, G_TRIP, nullptr),
            T(SIG_TRIP2, MSG_ODO, G_TRIP, nullptr),
            T(SIG_RANGE, MSG_TRIP, G_TRIP, nullptr),
            T(SIG_TRIP_ELAPSED, MSG_TRIP, G_TRIP, nullptr),

            T(SIG_FUEL_INST, MSG_FUEL_INST, G_FUEL, nullptr),
            T(SIG_FUEL_AVG, MSG_FUEL_HIST, G_FUEL, nullptr),
            T(SIG_FUEL_PCT, MSG_ODO, G_FUEL, nullptr),       // 0x7D 04 D4-D5, off by default

            T(SIG_ENG_TEMP, MSG_COOLANT, G_ENGINE, nullptr),
            X(SIG_BATT_V, G_ENGINE, nullptr, NF_BATT),

            T(SIG_OUT_TEMP, MSG_TEMP, G_CLIMATE, nullptr),
            T(SIG_AC_ON, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_COMPRESSOR, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_RECIRC, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_AUTO_HI, MSG_AC, G_CLIMATE, "AUTO"),
            T(SIG_AC_AUTO_LO, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_DUAL, MSG_AC, G_CLIMATE, "DUAL"),
            T(SIG_AC_MAX_FRONT, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_REAR, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_VENT_UP, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_VENT_FACE, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_VENT_FLOOR, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_DISPLAY_REQ, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_FAN, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_TEMP_DRV, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_TEMP_PASS, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_DEFOG, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_AC_AQS, MSG_AC, G_CLIMATE, "AQS"),
            T(SIG_AC_ECO, MSG_AC, G_CLIMATE, "ECO"),
            T(SIG_AC_UNIT_F, MSG_AC, G_CLIMATE, nullptr),
            T(SIG_SEAT_HEAT_DRV, MSG_SEATS, G_CLIMATE, nullptr),
            T(SIG_SEAT_HEAT_PASS, MSG_SEATS, G_CLIMATE, nullptr),
            T(SIG_SEAT_VENT_DRV, MSG_SEATS, G_CLIMATE, nullptr),
            T(SIG_SEAT_VENT_PASS, MSG_SEATS, G_CLIMATE, nullptr),

            T(SIG_LIGHT_POS, MSG_LIGHTS, G_LIGHTS, nullptr),
            T(SIG_LIGHT_DIPPED, MSG_LIGHTS, G_LIGHTS, nullptr),
            T(SIG_LIGHT_MAIN, MSG_LIGHTS, G_LIGHTS, nullptr),
            T(SIG_IND_LEFT, MSG_LIGHTS, G_LIGHTS, nullptr),
            T(SIG_IND_RIGHT, MSG_LIGHTS, G_LIGHTS, nullptr),

            T(SIG_RADAR_ENABLE, MSG_RADAR_REAR, G_RADAR, nullptr),
            T(SIG_RADAR_DISPLAY, MSG_RADAR_REAR, G_RADAR, nullptr),
            T(SIG_RADAR_FAR, MSG_RADAR_REAR, G_RADAR, nullptr),
            T(SIG_RADAR_VOLUME, MSG_RADAR_REAR, G_RADAR, nullptr),
            T(SIG_RADAR_RL, MSG_RADAR_REAR, G_RADAR, nullptr),
            T(SIG_RADAR_RLM, MSG_RADAR_REAR, G_RADAR, nullptr),
            T(SIG_RADAR_RRM, MSG_RADAR_REAR, G_RADAR, nullptr),
            T(SIG_RADAR_RR, MSG_RADAR_REAR, G_RADAR, nullptr),
            T(SIG_RADAR_FL, MSG_RADAR_FRONT, G_RADAR, nullptr),
            T(SIG_RADAR_FLM, MSG_RADAR_FRONT, G_RADAR, nullptr),
            T(SIG_RADAR_FRM, MSG_RADAR_FRONT, G_RADAR, nullptr),
            T(SIG_RADAR_FR, MSG_RADAR_FRONT, G_RADAR, nullptr),

            T(SIG_TPMS_PRESENT, MSG_TPMS, G_TYRES, nullptr),
            T(SIG_TPMS_ALARM, MSG_TPMS, G_TYRES, nullptr),
            T(SIG_TPMS_SHOW_SPARE, MSG_TPMS, G_TYRES, nullptr),
            T(SIG_TPMS_DIAGRAM, MSG_TPMS, G_TYRES, nullptr),
            T(SIG_TPMS_UNIT, MSG_TPMS, G_TYRES, nullptr),
            T(SIG_TPMS_FL, MSG_TPMS, G_TYRES, nullptr),
            T(SIG_TPMS_FR, MSG_TPMS, G_TYRES, nullptr),
            T(SIG_TPMS_RL, MSG_TPMS, G_TYRES, nullptr),
            T(SIG_TPMS_RR, MSG_TPMS, G_TYRES, nullptr),
            T(SIG_TPMS_SPARE, MSG_TPMS, G_TYRES, nullptr),

            T(SIG_AMP_FAD, MSG_AMP, G_AUDIO, nullptr),
            T(SIG_AMP_BAL, MSG_AMP, G_AUDIO, nullptr),
            T(SIG_AMP_BASS, MSG_AMP, G_AUDIO, nullptr),
            T(SIG_AMP_TREBLE, MSG_AMP, G_AUDIO, nullptr),
            T(SIG_AMP_MID, MSG_AMP, G_AUDIO, nullptr),
            T(SIG_AMP_ASL, MSG_AMP, G_AUDIO, "ASL"),
            T(SIG_AMP_VOLUME, MSG_AMP, G_AUDIO, nullptr),
            T(SIG_AMP_SDVC, MSG_AMP, G_AUDIO, nullptr),
            T(SIG_AMP_BEEP, MSG_AMP, G_AUDIO, nullptr),
            T(SIG_NODE_AMP, MSG_SYSTEM, G_AUDIO, nullptr),
            T(SIG_NODE_REAR, MSG_SYSTEM, G_AUDIO, nullptr),
            T(SIG_NODE_360, MSG_SYSTEM, G_AUDIO, nullptr),
            T(SIG_NODE_360_CUTIN, MSG_SYSTEM, G_AUDIO, nullptr),
            T(SIG_AMP_POWER, MSG_SYSTEM, G_AUDIO, nullptr),
            T(SIG_MUTE, MSG_SYSTEM, G_AUDIO, nullptr),

            T(SIG_SET_DRL, MSG_SETTINGS, G_SETTINGS, nullptr),
            T(SIG_SET_LAMP_SENS, MSG_SETTINGS, G_SETTINGS, nullptr),
            T(SIG_SET_ROOM_DELAY, MSG_SETTINGS, G_SETTINGS, nullptr),
            T(SIG_SET_LAMP_AUTOOFF, MSG_SETTINGS, G_SETTINGS, nullptr),
            T(SIG_SET_LOCK1, MSG_SETTINGS, G_SETTINGS, nullptr),
            T(SIG_SET_LOCK2, MSG_SETTINGS, G_SETTINGS, nullptr),
            T(SIG_SET_GUIDELINE, MSG_SETTINGS, G_SETTINGS, nullptr),
            T(SIG_SET_RELOCK, MSG_SETTINGS, G_SETTINGS, nullptr),
            T(SIG_ILLUM_DELAY, MSG_ILLUM, G_SETTINGS, nullptr),
            T(SIG_VSET_RADAR_DIST, MSG_VSET, G_SETTINGS, nullptr),
            T(SIG_VSET_FUEL_UNIT, MSG_VSET, G_SETTINGS, nullptr),
            T(SIG_VSET_THEME, MSG_VSET, G_SETTINGS, nullptr),

            T(SIG_HYBRID_PRESENT, MSG_HYBRID, G_HYBRID, nullptr),
            T(SIG_HYBRID_BATT, MSG_HYBRID, G_HYBRID, nullptr),
            T(SIG_HYBRID_FLOW, MSG_HYBRID, G_HYBRID, nullptr),

        };
        #undef T
        #undef X

        constexpr uint8_t kCount = (uint8_t)(sizeof(kRows) / sizeof(kRows[0]));
    }

    namespace RaiseTargets
    {
        uint8_t count() { return kCount; }

        TargetView at(uint8_t i)
        {
            const Row& r = kRows[i < kCount ? i : 0];
            const Meta& m = meta(r.sig);
            TargetView v;
            v.group   = kGroups[r.group];
            v.name    = r.name;
            v.cmd     = r.msg != NO_MSG ? kMsgCmd[r.msg] : "";
            v.why     = r.nofw != NF_NONE ? kNoFw[r.nofw] : nullptr;
            v.fw      = r.msg != NO_MSG;
            v.sig     = r.sig;
            v.msg     = r.msg;
            v.id      = m.name;
            v.unit    = m.unit;   // English and terse; the page has the words (k.unit.<unit>)
            v.boolean = m.boolean;
            v.min     = m.min;
            v.max     = m.max;
            v.dec     = m.decimals;
            return v;
        }
    }
}
