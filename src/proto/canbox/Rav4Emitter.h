#pragma once
#include <stdint.h>
#include "../../vehicle/VehicleState.h"
#include "../../Version.h"

// Everything this firmware can tell a RAV4-profile head unit, on a schedule
// (ARCHITECTURE.md §7.3). Portable: frames leave through a write callback, time
// arrives as nowMs, so the schedule is testable natively.
//
// A message is emitted only when its signals exist, decoded from CAN or pinned
// by hand; pinning lets a unit be probed without any vehicle CAN.
//
// A table: adding a message is a builder plus a row; the web UI picks it up.
namespace Canbox
{
    // Tuning::fuelUnit and tripUnit as the wire carries them, for /api/catalog.
    struct UnitRow { uint8_t v; const char* name; };
    inline constexpr UnitRow FUEL_UNITS[] = {{2, "л/100 км"}, {1, "км/л"}, {0, "mpg"}};
    inline constexpr UnitRow TRIP_UNITS[] = {{2, "км"}, {1, "милі"}, {0, "сховати"}};

    // Every value the protocol sources disagree on, settable from the web UI and
    // persisted (they get resolved in a car). Defaults are the best-attested reading.
    struct Tuning {
        // 0x28 carries two incompatible messages under one command byte and the
        // head unit drops the length it did not expect. Only one may be live.
        uint8_t cmd28        = 0;    // 0 = outside temp (12 B), 1 = A/C (5 B)

        // len 2, D1 = 0: the head unit's decoder reads D1 unconditionally (b7 =
        // bonnet), so with len 1 it reads the checksum byte there and shows the
        // bonnet open (docs/hu-decoder.md §2a). len 1 stays selectable.
        uint8_t doorsLen     = 2;    // 1 or 2

        // The head unit's own code negates before clamping; the vendor spec does
        // not. Decides which way the camera guidelines bend.
        uint8_t steerInvert  = 0;

        // Wire value = signal / steerDiv, clamped to +/- steerClamp.
        // The signal is degrees x10. steerDiv = 10 sends whole degrees, matching
        // the one verifiable worked example (2E 29 02 E6 FF EF = -26); steerDiv = 1
        // sends x10, as that document's prose ("0.1 deg units") says.
        // Clamp: spec +/-380, mikescotland +/-540 (a Clio III tracked best at
        // +/-500). Must never reach +/-5400.
        uint8_t steerDiv     = 10;
        int16_t steerClamp   = 540;

        // 0x22 D0 and 0x21 D6, named in FUEL_UNITS / TRIP_UNITS above.
        uint8_t fuelUnit     = 2;
        uint8_t tripUnit     = 2;
        // 0x21 D0..D3: always-zero padding per mikescotland, average speed and
        // elapsed minutes per the vendor spec. 1 = fill them, 0 = send zeros.
        uint8_t tripFields   = 1;
        // 0x23 length. 13 is the spec and populates the HISTORY screen; 3 is the
        // widely copied form that the unit's length guard drops.
        uint8_t histLen      = 13;
        // Radar distance polarity. Spec: 0 = no display, 1 = nearest .. 4 = farthest.
        // One reference emits the inverse. If the bars fill as you back AWAY, flip.
        uint8_t radarInvert  = 0;

        // 0x30, exactly 16 ASCII bytes on the wire; identifies the build on the
        // head unit's screen. Defaulted from Version.h; a booted board keeps
        // what it stored.
        char    version[17]  = MEGAOPEN_VERSION_WIRE;

        static constexpr uint8_t BLOB_SIZE = 32;
        void toBlob(uint8_t* out) const;
        bool fromBlob(const uint8_t* in, uint8_t len);
    };

    class Rav4Emitter {
    public:
        using WriteFn = void (*)(const uint8_t* frame, uint8_t len, void* ctx);

        // Message table index, public: the web UI and tests address messages by it.
        enum Msg : uint8_t {
            MSG_DOORS = 0,   // 0x24        doors (+ ignition, see setDoorsWithIgnition)
            MSG_TEMP,        // 0x28  12 B  outside temperature
            MSG_AC,          // 0x28   5 B  air conditioning — EXCLUSIVE with MSG_TEMP
            MSG_STEER,       // 0x29        steering angle
            MSG_LIGHTS,      // 0x7D 01     lights + indicators
            MSG_SPEED,       // 0x7D 03     road speed + average
            MSG_ODO,         // 0x7D 04     odometer + trip1/trip2
            MSG_RPM,         // 0x7D 0A     engine rpm
            MSG_TRIP,        // 0x21        trip computer / remaining range
            MSG_FUEL_INST,   // 0x22        instantaneous consumption
            MSG_FUEL_HIST,   // 0x23        history consumption
            MSG_RADAR_REAR,  // 0x1E        rear parking radar + radar master enable
            MSG_RADAR_FRONT, // 0x1D        front parking radar
            MSG_TPMS,        // 0x25        tyre pressures
            MSG_SETTINGS,    // 0x26        vehicle settings block
            MSG_AMP,         // 0x31        amplifier status
            MSG_SYSTEM,      // 0x32        node presence / mute
            MSG_ILLUM,       // 0x50        exterior illumination off-delay
            MSG_VSET,        // 0x52        vehicle-set return info
            MSG_SEATS,       // 0x58        seat heat / ventilation
            MSG_HYBRID,      // 0x1F        hybrid energy status
            MSG_VERSION,     // 0x30        version string
            MSG_COOLANT,     // 0x7D 09     coolant on the dashboard channel — EXP_COOLANT
            MSG_COUNT
        };

        // 0x28 carries two incompatible messages and the head unit drops a frame
        // of unexpected length, so only one may be scheduled. 12-byte outside
        // temp is what the DUDU displays for us (attested by mikescotland); 5-byte
        // A/C is the vendor spec and what the head-unit Java parses.
        enum class Cmd28 : uint8_t { OutsideTemp = 0, AirCon = 1 };

        void begin(WriteFn write, void* ctx) { _write = write; _ctx = ctx; }
        void setEnabled(bool on) { _enabled = on; }
        bool enabled() const { return _enabled; }

        // The uncertain-value block: what the web UI edits and NVS stores.
        void setTuning(const Tuning& t) { _t = t; }
        const Tuning& tuning() const { return _t; }

        void setCmd28(Cmd28 m) { _t.cmd28 = (m == Cmd28::AirCon) ? 1 : 0; }
        Cmd28 cmd28() const { return _t.cmd28 ? Cmd28::AirCon : Cmd28::OutsideTemp; }

        // 0x24 length. mikescotland (source of our 0x7D messages, proven on this
        // unit) sends len 1, doors only. The vendor spec sends len 2 with the
        // IGNITION bit in D1 b1, which gates the trip, fuel, history and TPMS
        // panels. A wrong length is dropped, not degraded.
        // Default len 2 (see Tuning::doorsLen).
        void setDoorsWithIgnition(bool on) { _t.doorsLen = on ? 2 : 1; }
        bool doorsWithIgnition() const { return _t.doorsLen == 2; }

        // Negate steering before clamping (head unit code) or not (vendor spec).
        void setSteeringInverted(bool inv) { _t.steerInvert = inv ? 1 : 0; }
        bool steeringInverted() const { return _t.steerInvert != 0; }

        // Per-message schedule switch, for taking a pinned message off the
        // wire. sendNow() ignores it.
        void setMsgEnabled(uint8_t i, bool on);
        bool msgEnabled(uint8_t i) const;

        // Run every scheduler against the current state.
        void tick(const Vehicle::State& s, uint32_t nowMs);

        // Emit one message now, regardless of schedule (web UI "send now").
        bool sendNow(uint8_t msgIndex, const Vehicle::State& s);

        // The 0x90 responder: the head unit names a box->HU command it wants
        // re-sent. False when there is no builder or nothing to say.
        bool sendByCmd(uint8_t cmd, const Vehicle::State& s);

        static uint8_t messageCount();
        static const char* messageName(uint8_t i);
        static uint8_t messageCmd(uint8_t i);     // the wire command byte
        static uint32_t messageInterval(uint8_t i);

        static uint8_t doorsMask(const Vehicle::State& s);
        static uint8_t lightsMask(const Vehicle::State& s);

        uint32_t framesSent() const { return _framesSent; }

        // Build one message into a caller-owned buffer without transmitting it.
        bool preview(uint8_t msgIndex, const Vehicle::State& s,
                     uint8_t* frame, uint8_t& frameLen) const;

        static constexpr uint8_t MAX_MESSAGES = 24;

        // Experiments on fields the head unit decodes but no capture has shown
        // in use (docs/hu-decoder.md §2b, §6). Each is its own switch, all OFF
        // by default (NVS "huExp"): only the tablet can say what they show.
        // A test send from the map page ignores them, like the schedule.
        enum Exp : uint8_t {
            EXP_COOLANT  = 1 << 0,   // schedule 0x7D 09 [00][t+40] (1049 idx 2)
            EXP_FUEL_7D04 = 1 << 1,  // 0x7D 04 D4-D5 = fuel litres x10, not F2 08 (1049 idx 5)
            EXP_COMPUTED = 1 << 2,   // range / consumption from Derived when no
                                     // range / fuel_inst / fuel_avg is bound
            EXP_ALL      = 0x07,
        };
        void    setExperiments(uint8_t m) { _exp = m & EXP_ALL; }
        uint8_t experiments() const { return _exp; }

        Rav4Emitter();

    private:
        struct Sched { uint32_t lastMs = 0; uint32_t lastVal = 0xFFFFFFFF; };

        void emit(uint8_t cmd, const uint8_t* payload, uint8_t len);
        bool due(Sched& s, uint32_t nowMs, uint32_t interval, uint32_t value, bool onChange);
        bool build(uint8_t msgIndex, const Vehicle::State& s,
                   uint8_t& cmd, uint8_t* payload, uint8_t& len) const;
        uint8_t radar(int32_t v) const;   // distance + the polarity switch
        // `sig` when it has a value; else, under EXP_COMPUTED, `computed`;
        // else SIG_COUNT.
        uint8_t pick(const Vehicle::State& s, uint8_t sig, uint8_t computed) const;

        WriteFn  _write = nullptr;
        void*    _ctx = nullptr;
        bool     _enabled = true;
        uint8_t  _exp = 0;               // Exp bits
        Tuning   _t;                    // every value the sources disagree about
        uint32_t _framesSent = 0;
        bool     _msgOn[MAX_MESSAGES];
        Sched    _sched[MAX_MESSAGES];
    };
}
