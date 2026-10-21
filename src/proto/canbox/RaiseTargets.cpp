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
        const char* const kGroups[] = {"Двері", "Рух", "Пробіг", "Паливо", "Двигун", "Клімат", "Світло", "Радар", "Шини", "Звук", "Налаштування", "Гібрид"};

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
        const char* const kNoFw[] = {
            // 0x41 02 is VW's; this head unit decodes 0x41 into Toyota codes
            // and 0x7D 0B not at all (docs/hu-decoder.md §2b-2c). EXP_BATT
            // borrows another field instead.
            "протокол 112 цього не має",
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
            T(SIG_DOOR_FL, MSG_DOORS, G_DOORS, "Передні ліві"),
            T(SIG_DOOR_FR, MSG_DOORS, G_DOORS, "Передні праві"),
            T(SIG_DOOR_RL, MSG_DOORS, G_DOORS, "Задні ліві"),
            T(SIG_DOOR_RR, MSG_DOORS, G_DOORS, "Задні праві"),
            T(SIG_BOOT, MSG_DOORS, G_DOORS, "Багажник"),
            T(SIG_KEY_ON, MSG_DOORS, G_DOORS, "Запалювання (D1 b1)"),
            T(SIG_BONNET, MSG_DOORS, G_DOORS, "Капот (D1 b7)"),

            T(SIG_SPEED, MSG_SPEED, G_DRIVE, "Швидкість"),
            T(SIG_AVG_SPEED, MSG_TRIP, G_DRIVE, "Середня швидкість"),   // also 7D 03, which needs speed
            T(SIG_RPM, MSG_RPM, G_DRIVE, "Оберти"),
            T(SIG_STEERING, MSG_STEER, G_DRIVE, "Кут керма"),
            T(SIG_PRND, MSG_GEAR, G_DRIVE, "Селектор P R N D (проба, експеримент)"),

            T(SIG_ODOMETER, MSG_ODO, G_TRIP, "Одометр"),
            T(SIG_TRIP1, MSG_ODO, G_TRIP, "Поїздка 1"),
            T(SIG_TRIP2, MSG_ODO, G_TRIP, "Поїздка 2"),
            T(SIG_RANGE, MSG_TRIP, G_TRIP, "Запас ходу"),
            T(SIG_TRIP_ELAPSED, MSG_TRIP, G_TRIP, "Час у дорозі"),

            T(SIG_FUEL_INST, MSG_FUEL_INST, G_FUEL, "Миттєва витрата"),
            T(SIG_FUEL_AVG, MSG_FUEL_HIST, G_FUEL, "Витрата за поїздку"),

            T(SIG_ENG_TEMP, MSG_COOLANT, G_ENGINE, "t° охолодної рідини (експеримент)"),
            X(SIG_BATT_V, G_ENGINE, "Напруга АКБ", NF_BATT),

            T(SIG_OUT_TEMP, MSG_TEMP, G_CLIMATE, "t° зовні"),
            T(SIG_AC_ON, MSG_AC, G_CLIMATE, "Кондиціонер"),
            T(SIG_AC_COMPRESSOR, MSG_AC, G_CLIMATE, "Компресор"),
            T(SIG_AC_RECIRC, MSG_AC, G_CLIMATE, "Рециркуляція"),
            T(SIG_AC_AUTO_HI, MSG_AC, G_CLIMATE, "AUTO"),
            T(SIG_AC_AUTO_LO, MSG_AC, G_CLIMATE, "AUTO, друга лампа"),
            T(SIG_AC_DUAL, MSG_AC, G_CLIMATE, "DUAL"),
            T(SIG_AC_MAX_FRONT, MSG_AC, G_CLIMATE, "MAX спереду"),
            T(SIG_AC_REAR, MSG_AC, G_CLIMATE, "Задній клімат"),
            T(SIG_AC_VENT_UP, MSG_AC, G_CLIMATE, "Обдув: скло"),
            T(SIG_AC_VENT_FACE, MSG_AC, G_CLIMATE, "Обдув: обличчя"),
            T(SIG_AC_VENT_FLOOR, MSG_AC, G_CLIMATE, "Обдув: ноги"),
            T(SIG_AC_DISPLAY_REQ, MSG_AC, G_CLIMATE, "Показати панель клімату"),
            T(SIG_AC_FAN, MSG_AC, G_CLIMATE, "Вентилятор"),
            T(SIG_AC_TEMP_DRV, MSG_AC, G_CLIMATE, "Уставка водія, байт"),
            T(SIG_AC_TEMP_PASS, MSG_AC, G_CLIMATE, "Уставка пасажира, байт"),
            T(SIG_AC_DEFOG, MSG_AC, G_CLIMATE, "Обдув скла"),
            T(SIG_AC_AQS, MSG_AC, G_CLIMATE, "AQS"),
            T(SIG_AC_ECO, MSG_AC, G_CLIMATE, "ECO"),
            T(SIG_AC_UNIT_F, MSG_AC, G_CLIMATE, "°F замість °C"),
            T(SIG_SEAT_HEAT_DRV, MSG_SEATS, G_CLIMATE, "Підігрів сидіння водія"),
            T(SIG_SEAT_HEAT_PASS, MSG_SEATS, G_CLIMATE, "Підігрів сидіння пасажира"),
            T(SIG_SEAT_VENT_DRV, MSG_SEATS, G_CLIMATE, "Вентиляція сидіння водія"),
            T(SIG_SEAT_VENT_PASS, MSG_SEATS, G_CLIMATE, "Вентиляція сидіння пасажира"),

            T(SIG_LIGHT_POS, MSG_LIGHTS, G_LIGHTS, "Габарити"),
            T(SIG_LIGHT_DIPPED, MSG_LIGHTS, G_LIGHTS, "Ближнє"),
            T(SIG_LIGHT_MAIN, MSG_LIGHTS, G_LIGHTS, "Дальнє"),
            T(SIG_IND_LEFT, MSG_LIGHTS, G_LIGHTS, "Поворот ◀"),
            T(SIG_IND_RIGHT, MSG_LIGHTS, G_LIGHTS, "Поворот ▶"),

            T(SIG_RADAR_ENABLE, MSG_RADAR_REAR, G_RADAR, "Радар увімкнено"),
            T(SIG_RADAR_DISPLAY, MSG_RADAR_REAR, G_RADAR, "Показати радар"),
            T(SIG_RADAR_FAR, MSG_RADAR_REAR, G_RADAR, "Далека межа"),
            T(SIG_RADAR_VOLUME, MSG_RADAR_REAR, G_RADAR, "Гучність радара"),
            T(SIG_RADAR_RL, MSG_RADAR_REAR, G_RADAR, "Задній лівий"),
            T(SIG_RADAR_RLM, MSG_RADAR_REAR, G_RADAR, "Задній лівий середній"),
            T(SIG_RADAR_RRM, MSG_RADAR_REAR, G_RADAR, "Задній правий середній"),
            T(SIG_RADAR_RR, MSG_RADAR_REAR, G_RADAR, "Задній правий"),
            T(SIG_RADAR_FL, MSG_RADAR_FRONT, G_RADAR, "Передній лівий"),
            T(SIG_RADAR_FLM, MSG_RADAR_FRONT, G_RADAR, "Передній лівий середній"),
            T(SIG_RADAR_FRM, MSG_RADAR_FRONT, G_RADAR, "Передній правий середній"),
            T(SIG_RADAR_FR, MSG_RADAR_FRONT, G_RADAR, "Передній правий"),

            T(SIG_TPMS_PRESENT, MSG_TPMS, G_TYRES, "TPMS є"),
            T(SIG_TPMS_ALARM, MSG_TPMS, G_TYRES, "Тривога"),
            T(SIG_TPMS_SHOW_SPARE, MSG_TPMS, G_TYRES, "Показати запаску"),
            T(SIG_TPMS_DIAGRAM, MSG_TPMS, G_TYRES, "Схема авто"),
            T(SIG_TPMS_UNIT, MSG_TPMS, G_TYRES, "Одиниця тиску"),
            T(SIG_TPMS_FL, MSG_TPMS, G_TYRES, "Тиск переднє ліве, байт"),
            T(SIG_TPMS_FR, MSG_TPMS, G_TYRES, "Тиск переднє праве, байт"),
            T(SIG_TPMS_RL, MSG_TPMS, G_TYRES, "Тиск заднє ліве, байт"),
            T(SIG_TPMS_RR, MSG_TPMS, G_TYRES, "Тиск заднє праве, байт"),
            T(SIG_TPMS_SPARE, MSG_TPMS, G_TYRES, "Тиск запаски, байт"),

            T(SIG_AMP_FAD, MSG_AMP, G_AUDIO, "Фейдер"),
            T(SIG_AMP_BAL, MSG_AMP, G_AUDIO, "Баланс"),
            T(SIG_AMP_BASS, MSG_AMP, G_AUDIO, "Низькі"),
            T(SIG_AMP_TREBLE, MSG_AMP, G_AUDIO, "Високі"),
            T(SIG_AMP_MID, MSG_AMP, G_AUDIO, "Середні"),
            T(SIG_AMP_ASL, MSG_AMP, G_AUDIO, "ASL"),
            T(SIG_AMP_VOLUME, MSG_AMP, G_AUDIO, "Гучність"),
            T(SIG_AMP_SDVC, MSG_AMP, G_AUDIO, "Гучність від швидкості"),
            T(SIG_AMP_BEEP, MSG_AMP, G_AUDIO, "Звук кнопок"),
            T(SIG_NODE_AMP, MSG_SYSTEM, G_AUDIO, "Підсилювач є"),
            T(SIG_NODE_REAR, MSG_SYSTEM, G_AUDIO, "Задній блок є"),
            T(SIG_NODE_360, MSG_SYSTEM, G_AUDIO, "Камера 360 є"),
            T(SIG_NODE_360_CUTIN, MSG_SYSTEM, G_AUDIO, "Камера 360 вмикається сама"),
            T(SIG_AMP_POWER, MSG_SYSTEM, G_AUDIO, "Підсилювач увімкнено"),
            T(SIG_MUTE, MSG_SYSTEM, G_AUDIO, "Без звуку"),

            T(SIG_SET_DRL, MSG_SETTINGS, G_SETTINGS, "Денні ходові"),
            T(SIG_SET_LAMP_SENS, MSG_SETTINGS, G_SETTINGS, "Чутливість датчика світла"),
            T(SIG_SET_ROOM_DELAY, MSG_SETTINGS, G_SETTINGS, "Затримка світла в салоні"),
            T(SIG_SET_LAMP_AUTOOFF, MSG_SETTINGS, G_SETTINGS, "Автовимкнення фар"),
            T(SIG_SET_LOCK1, MSG_SETTINGS, G_SETTINGS, "Замки, байт 1"),
            T(SIG_SET_LOCK2, MSG_SETTINGS, G_SETTINGS, "Замки, байт 2"),
            T(SIG_SET_GUIDELINE, MSG_SETTINGS, G_SETTINGS, "Лінії камери"),
            T(SIG_SET_RELOCK, MSG_SETTINGS, G_SETTINGS, "Повторне замикання"),
            T(SIG_ILLUM_DELAY, MSG_ILLUM, G_SETTINGS, "Затримка підсвітки"),
            T(SIG_VSET_RADAR_DIST, MSG_VSET, G_SETTINGS, "Радар: відстань"),
            T(SIG_VSET_FUEL_UNIT, MSG_VSET, G_SETTINGS, "Витрата в км/л"),
            T(SIG_VSET_THEME, MSG_VSET, G_SETTINGS, "Тема"),

            T(SIG_HYBRID_PRESENT, MSG_HYBRID, G_HYBRID, "Гібрид є"),
            T(SIG_HYBRID_BATT, MSG_HYBRID, G_HYBRID, "Заряд батареї"),
            T(SIG_HYBRID_FLOW, MSG_HYBRID, G_HYBRID, "Потоки енергії, біти"),

        };
        #undef T
        #undef X

        constexpr uint8_t kCount = (uint8_t)(sizeof(kRows) / sizeof(kRows[0]));

        // Signals.cpp units are terse and English; the page shows these. A
        // range ("0–7") is a hint, and the page leaves it out of a value.
        struct Unit { const char* en; const char* uk; };
        const Unit kUnits[] = {
            {"km/h", "км/год"}, {"rpm", "об/хв"}, {"deg", "°"}, {"km", "км"}, {"C", "°C"},
            {"min", "хв"}, {"L/100km", "л/100 км"}, {"raw", "байт"}, {"0-2", "0–2"},
            {"0-3", "0–3"}, {"0-4", "0–4"}, {"0-7", "0–7"}, {"0-8", "0–8"}, {"0-14", "0–14"},
            {"0-15", "0–15"}, {"0-63", "0–63"}, {"1-5", "1–5"}, {"-5..5", "−5–5"}, {"V", "В"},
        };

        const char* unitUk(const char* en)
        {
            for (const Unit& u : kUnits) if (!strcmp(u.en, en)) return u.uk;
            return en;
        }
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
            v.unit    = unitUk(m.unit);
            v.boolean = m.boolean;
            v.min     = m.min;
            v.max     = m.max;
            v.dec     = m.decimals;
            return v;
        }

        int find(const char* id)
        {
            if (!id) return -1;
            for (uint8_t i = 0; i < kCount; ++i)
                if (!strcmp(at(i).id, id)) return i;
            return -1;
        }

        const char* fmt(char* b, unsigned cap, int32_t v, uint8_t dec)
        {
            if (!dec) { snprintf(b, cap, "%ld", (long)v); return b; }
            int32_t p = 1;
            for (uint8_t i = 0; i < dec; ++i) p *= 10;
            const int64_t a = v < 0 ? -(int64_t)v : v;
            snprintf(b, cap, "%s%ld.%0*ld", v < 0 ? "-" : "", (long)(a / p), (int)dec, (long)(a % p));
            return b;
        }

        bool toInt(double v, uint8_t dec, int32_t& out)
        {
            for (uint8_t i = 0; i < dec; ++i) v *= 10;
            if (!(v > -2147483648.0 && v < 2147483647.0)) return false;   // NaN too
            out = (int32_t)lround(v);
            return true;
        }
    }
}
