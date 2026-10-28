#include "Raise112.h"
#include <new>
#include <string.h>
#include "RaiseFramer.h"
#include "RaiseTargets.h"
#include "Rav4Catalog.h"
#include "Rav4Emitter.h"
#include "../../cfg/SettingsRegistry.h"

namespace Canbox
{
    namespace
    {
        using Cfg::Model;
        using Cfg::SEntry;
        using Cfg::SEnum;
        using Cfg::SStr;
        using Cfg::SType;

        const Tuning& tun(const Model& m) { return *static_cast<const Tuning*>(m.hu); }
        Tuning&       tun(Model& m)       { return *static_cast<Tuning*>(m.hu); }

        // ---- the settings rows (raise.*), HuConfig::tuning as a Tuning ----------
        // 0x28 carries one of two messages: the row is the stored value.
        const char* const kCmd28[] = {"temp", "ac"};
        // FUEL_UNITS / TRIP_UNITS rows, with ASCII ids; the row stores its `v`.
        const char* const kFuelIds[] = {"l100km", "kml", "mpg"};
        const char* const kTripIds[] = {"km", "miles", "hide"};
        static_assert(sizeof(kFuelIds) / sizeof(kFuelIds[0]) == sizeof(FUEL_UNITS) / sizeof(FUEL_UNITS[0]),
                      "kFuelIds is out of step with FUEL_UNITS");
        static_assert(sizeof(kTripIds) / sizeof(kTripIds[0]) == sizeof(TRIP_UNITS) / sizeof(TRIP_UNITS[0]),
                      "kTripIds is out of step with TRIP_UNITS");
        constexpr uint8_t FUEL_ROWS = sizeof(kFuelIds) / sizeof(kFuelIds[0]);
        constexpr uint8_t TRIP_ROWS = sizeof(kTripIds) / sizeof(kTripIds[0]);

        const SEnum kCmd28E = {
            [] { return (uint8_t)2; },
            [](uint8_t i) { return kCmd28[i < 2 ? i : 0]; },
            [](uint8_t) -> const char* { return nullptr; },
            "cmd28",
        };
        const SEnum kFuelE = {
            [] { return FUEL_ROWS; },
            [](uint8_t i) { return kFuelIds[i < FUEL_ROWS ? i : 0]; },
            [](uint8_t i) { return FUEL_UNITS[i < FUEL_ROWS ? i : 0].name; },
            "fuelUnit",
        };
        const SEnum kTripE = {
            [] { return TRIP_ROWS; },
            [](uint8_t i) { return kTripIds[i < TRIP_ROWS ? i : 0]; },
            [](uint8_t i) { return TRIP_UNITS[i < TRIP_ROWS ? i : 0].name; },
            "tripUnit",
        };
        template <size_t N>
        int32_t unitRow(const UnitRow (&t)[N], uint8_t v)
        {
            for (size_t i = 0; i < N; ++i) if (t[i].v == v) return (int32_t)i;
            return 0;
        }

        // 0x30: exactly 16 bytes on the wire, space-padded; shown trimmed.
        constexpr uint8_t VERSION_LEN = sizeof(Tuning{}.version) - 1;
        const SStr kVersion = {
            VERSION_LEN,
            [](const Model& m, char* out, size_t cap) {
                if (!cap) return;
                const Tuning& t = tun(m);
                size_t n = strnlen(t.version, VERSION_LEN);
                while (n && t.version[n - 1] == ' ') --n;
                if (n >= cap) n = cap - 1;
                memcpy(out, t.version, n);
                out[n] = 0;
            },
            [](Model& m, const char* v) {
                Tuning& t = tun(m);
                const size_t n = strnlen(v, VERSION_LEN);
                for (size_t i = 0; i < VERSION_LEN; ++i) t.version[i] = i < n ? v[i] : ' ';
                t.version[VERSION_LEN] = 0;
            },
        };

#define T_GET(expr) [](const Model& m) -> int32_t { const Tuning& t = tun(m); return (int32_t)(expr); }
#define T_PUT(stmt) [](Model& m, int32_t v) { Tuning& t = tun(m); (void)v; stmt; }
        constexpr uint8_t W = Cfg::F_WEB, HU = Cfg::AP_HU;

        // The ranges only stop nonsense: the emitter takes any of these
        // (docs/raise-protocol.md says what each one changes on the unit).
        const SEntry kRows[] = {
            {"raise", "Raise", nullptr, SType::Group, W, Cfg::AP_NONE,
             0, 0, 0, nullptr, nullptr, nullptr, nullptr},
            {"raise.doorsLen", "Doors len", "raise", SType::Int, W, HU,
             1, 2, 1, nullptr, nullptr, T_GET(t.doorsLen), T_PUT(t.doorsLen = (uint8_t)v)},
            {"raise.cmd28", "0x28 carries", "raise", SType::Enum, W, HU,
             0, 0, 0, nullptr, &kCmd28E, T_GET(t.cmd28 ? 1 : 0), T_PUT(t.cmd28 = (uint8_t)v)},
            {"raise.steerInvert", "Steer invert", "raise", SType::Bool, W, HU,
             0, 1, 0, nullptr, nullptr, T_GET(t.steerInvert != 0), T_PUT(t.steerInvert = v ? 1 : 0)},
            // 10 = whole degrees, 1 = the x10 value.
            {"raise.steerDiv", "Steer div", "raise", SType::Int, W, HU,
             1, 10, 1, nullptr, nullptr, T_GET(t.steerDiv), T_PUT(t.steerDiv = (uint8_t)v)},
            // Spec 380, a Clio III 500, mikescotland 540; never near 5400.
            {"raise.steerClamp", "Steer clamp", "raise", SType::Int, W, HU,
             100, 1000, 10, nullptr, nullptr, T_GET(t.steerClamp), T_PUT(t.steerClamp = (int16_t)v)},
            {"raise.fuelUnit", "Fuel unit", "raise", SType::Enum, W, HU,
             0, 0, 0, nullptr, &kFuelE, T_GET(unitRow(FUEL_UNITS, t.fuelUnit)),
             T_PUT(t.fuelUnit = FUEL_UNITS[v].v)},
            {"raise.tripUnit", "Trip unit", "raise", SType::Enum, W, HU,
             0, 0, 0, nullptr, &kTripE, T_GET(unitRow(TRIP_UNITS, t.tripUnit)),
             T_PUT(t.tripUnit = TRIP_UNITS[v].v)},
            {"raise.tripFields", "Trip fields", "raise", SType::Bool, W, HU,
             0, 1, 0, nullptr, nullptr, T_GET(t.tripFields != 0), T_PUT(t.tripFields = v ? 1 : 0)},
            // 13 fills the HISTORY screen; 3 is the copied form the unit drops.
            {"raise.histLen", "History len", "raise", SType::Int, W, HU,
             3, 13, 1, nullptr, nullptr, T_GET(t.histLen), T_PUT(t.histLen = (uint8_t)v)},
            {"raise.radarInvert", "Radar invert", "raise", SType::Bool, W, HU,
             0, 1, 0, nullptr, nullptr, T_GET(t.radarInvert != 0), T_PUT(t.radarInvert = v ? 1 : 0)},
            {"raise.version", "Version", "raise", SType::Str, W, HU,
             0, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr, &kVersion},
        };
#undef T_GET
#undef T_PUT
        constexpr size_t ROWS = sizeof(kRows) / sizeof(kRows[0]);

        struct Raise112 final : Protocol {
            const char* id() const override { return "raise112"; }

            uint8_t     fieldCount() const override { return RaiseTargets::count(); }
            TargetView  field(uint8_t i) const override { return RaiseTargets::at(i); }
            const FieldMap& defaults() const override { return Rav4Emitter::defaults(); }

            // 0x28 carries one form at a time: a field in the other is not sent.
            const char* messageBlocked(uint8_t msg, const HuConfig& c) const override
            {
                const uint8_t cmd28 = c.tuningAs<Tuning>().cmd28;
                if (msg == Rav4Emitter::MSG_TEMP && cmd28)  return "form_ac";
                if (msg == Rav4Emitter::MSG_AC && !cmd28)   return "form_temp";
                return nullptr;
            }

            uint8_t     messageCount() const override { return Rav4Emitter::messageCount(); }
            MessageView message(uint8_t i) const override
            {
                const MsgInfo& info = msgInfo(i);
                return {Rav4Emitter::messageName(i), Rav4Emitter::messageCmd(i),
                        Rav4Emitter::messageInterval(i), confName(info.conf), info.note};
            }

            size_t   emitterSize() const override { return sizeof(Rav4Emitter); }
            Emitter* makeEmitter(void* mem) const override { return new (mem) Rav4Emitter(); }

            uint8_t frame(uint8_t cmd, const uint8_t* payload, uint8_t len, uint8_t* out) const override
            {
                return buildFrame(cmd, payload, len, out);
            }

            void        initTuning(void* t) const override { new (t) Tuning(); }
            const char* tuningKey() const override { return "rav4tune"; }
            uint8_t     tuningBlobSize() const override { return Tuning::BLOB_SIZE; }
            void tuningToBlob(const void* t, uint8_t* out) const override
            {
                static_cast<const Tuning*>(t)->toBlob(out);
            }
            bool tuningFromBlob(void* t, const uint8_t* in, uint8_t len) const override
            {
                Tuning n;
                if (!n.fromBlob(in, len)) return false;
                *static_cast<Tuning*>(t) = n;
                return true;
            }

            size_t        settingCount() const override { return ROWS; }
            const SEntry& setting(size_t i) const override { return kRows[i < ROWS ? i : 0]; }
        };
        static_assert(sizeof(Tuning) <= HuConfig::TUNING_MAX, "Tuning must fit HuConfig::TUNING_MAX");
        static_assert(alignof(Tuning) <= 8, "HuConfig::tuning is 8-aligned");
    }

    const Protocol& raise112()
    {
        static const Raise112 p;
        return p;
    }
}
