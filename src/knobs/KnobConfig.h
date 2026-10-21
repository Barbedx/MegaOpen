#pragma once
#include <stdint.h>
#include "../keys/KeyAction.h"
#include "../utils/UiList.h"

// The two front-panel knobs and their backlight, as one document. Portable so
// the web page, NVS and the native tests share the structure.
//
//   RIGHT  a quadrature encoder with a push button (S1, S2, KEY)
//   LEFT   a second encoder (was a potentiometer; pot fields kept for old blobs)
//
// Everything that can emit keys starts off: the DevKit encoder inputs are
// input-only pins without pull-ups, and floating inputs would become key
// presses at the head unit. Same reasoning as routeRes in Cfg::Settings.
// The one exception: knobs behind a PCF8574 (Board::KNOBS_I2C, the T-2CAN)
// cannot float, so there the encoders start ON (KnobConfig.cpp).
namespace Knobs
{
    struct Binding {
        Keys::Action action = Keys::Action::None;
        uint8_t      code   = 0;     // 0 = let the sink use its profile's code
    };

    enum class PotMode : uint8_t {
        Off = 0,
        Steps,          // crossing a zone emits up/down — a volume knob with end stops
        Brightness,     // the travel is the backlight dimmer; no key events at all
        _Count
    };

    // What a knob's LEDs do when it moves.
    enum class React : uint8_t {
        None = 0,       // nothing: the resting look, always
        Level,          // the whole zone runs a gradient lo -> hi with the knob
        Flash,          // the resting look, kicked brighter by every click
        // One LED lit `hi`, the rest dim; the lit one walks with the knob (a
        // click per LED). With one LED: `hi` while moving.
        Spin,
        // Each click sends one dot round the ring in the turn direction
        // (five LEDs in 150 ms).
        Sweep,
        // Spin with a tail fading through the resting colour.
        Trail,
        _Count
    };

    // What a zone looks like when nobody is touching it.
    enum class Anim : uint8_t {
        Static = 0,     // each LED at its own colour
        Breathe,        // the same colours, swelling and fading
        Rainbow,        // the wheel, spread across the zone; the colours are ignored
        _Count
    };

    // The strip's chips, by their wire order. WS2812B is GRB; SK6812 RGBW
    // strips take four bytes a LED, G R B W.
    enum class Order : uint8_t { GRB = 0, RGB, GRBW, _Count };

    inline constexpr uint8_t bytesPerLed(Order o) { return o == Order::GRBW ? 4 : 3; }

    // `count` R,G,B triplets into the chips' wire bytes; returns how many
    // bytes. RGBW: the white LED carries what the three have in common,
    // w = min(r, g, b), taken off each, so a colour keeps its hue and white
    // is the white LED's own (the RGB mix of it is bluish and costs three
    // channels). `out` holds count * bytesPerLed(o). Pure: host-tested.
    inline uint16_t packWire(const uint8_t* rgb, uint8_t count, Order o, uint8_t* out)
    {
        uint16_t n = 0;
        for (uint16_t i = 0; i < (uint16_t)count * 3; i += 3) {
            const uint8_t r = rgb[i], g = rgb[i + 1], b = rgb[i + 2];
            if (o == Order::RGB) { out[n++] = r; out[n++] = g; out[n++] = b; continue; }
            if (o == Order::GRB) { out[n++] = g; out[n++] = r; out[n++] = b; continue; }
            const uint8_t w = r < g ? (r < b ? r : b) : (g < b ? g : b);
            out[n++] = (uint8_t)(g - w); out[n++] = (uint8_t)(r - w);
            out[n++] = (uint8_t)(b - w); out[n++] = w;
        }
        return n;
    }

    struct Rgb {
        uint8_t r = 0, g = 0, b = 0;
    };

    inline constexpr uint8_t ZONE_LEFT  = 0;    // the pot
    inline constexpr uint8_t ZONE_RIGHT = 1;    // the encoder
    inline constexpr uint8_t ZONES      = 2;
    inline constexpr uint8_t ZONE_LEDS  = 8;    // behind one knob
    // The chain continues past the knobs into the footwells; they come last.
    inline constexpr uint8_t FEET_LEDS  = 144;
    inline constexpr uint8_t MAX_LEDS   = ZONES * ZONE_LEDS + FEET_LEDS;

    // WS2812B draw for the current limiter: 20 mA per channel at full duty
    // (datasheet, errs high), ~1 mA while black.
    inline constexpr uint16_t LED_MA_PER_CHANNEL = 20;
    inline constexpr uint16_t LED_MA_IDLE        = 1;

    // The LEDs behind one knob. Zones are consecutive on one chain.
    struct Zone {
        uint8_t count = 1;
        React   react = React::Level;
        Anim    anim  = Anim::Static;
        // Per-LED colours; all eight stored whatever `count` says.
        Rgb     led[ZONE_LEDS];
        Rgb     lo, hi;     // Level: the gradient's ends. Spin: `hi` is the lit one.
    };

    // One quadrature encoder with a push button.
    struct Enc {
        bool     enabled  = false;
        bool     inverted = false;
        uint8_t  ppd      = 4;                  // quarter steps per click: 4, 2 or 1
        Binding  cw, ccw, click, hold;
        uint16_t holdMs   = 600;
    };
    inline constexpr uint8_t ENC_RIGHT = 0;     // prev / next / pause
    inline constexpr uint8_t ENC_LEFT  = 1;     // volume
    inline constexpr uint8_t ENCODERS  = 2;

    struct Config {
        // ---- the encoders. Index 0 is the right knob, 1 the left ---------------
        Enc      enc[ENCODERS];

        // Legacy accessor: means the right knob.
        bool&    encEnabled()        { return enc[ENC_RIGHT].enabled; }
        bool     encEnabled()  const { return enc[ENC_RIGHT].enabled; }

        // ---- left: potentiometer (RETIRED, kept so an old blob still loads) ----
        PotMode  potMode     = PotMode::Off;
        bool     potInverted = false;
        uint8_t  potZones    = 30;
        uint16_t potMin      = 0;
        uint16_t potMax      = 4095;
        Binding  potUp   {Keys::Action::VolUp,   0};
        Binding  potDown {Keys::Action::VolDown, 0};

        // ---- backlight ----------------------------------------------------------
        bool     ledEnabled  = false;
        Order    order       = Order::GRB;
        uint8_t  brightness  = 128;
        // How long a reaction colour outlives the last movement. 0 = it stays.
        uint16_t reactHoldMs = 1500;
        // One Breathe / Rainbow cycle, in tenths of a second.
        uint8_t  animPeriod  = 60;
        // The encoder's gradient runs over a virtual position this many clicks
        // long, clamped at both ends.
        uint8_t  levelSteps  = 30;
        // Which knob's LEDs come first on the chain (wiring).
        bool     rightFirst  = false;
        // The LEDs of each ring run anticlockwise seen from the front (wiring):
        // every effect that moves round a ring is drawn mirrored.
        bool     ringReverse = false;
        Zone     zone[ZONES];

        // ---- footwells ----------------------------------------------------------
        // One colour for all of them.
        uint8_t  feetCount      = 0;
        Anim     feetAnim       = Anim::Static;
        Rgb      feetColor;
        // Own dimmer, applied under the master brightness.
        uint8_t  feetBrightness = 96;

        // ---- the supply ----------------------------------------------------------
        // Chain current cap, enforced by scaling each frame. Defaults to a USB
        // port's budget: on a DevKit, VIN LED current goes through the board's
        // 1 A Schottky. Raise it when the strip has its own supply.
        uint16_t maxMilliamps   = 400;

        Config();
        // Brings every field into its limits; returns which groups it had to
        // change (CLAMP_* bits), so a settings file's preview can say so.
        uint16_t sanitize();
        enum : uint16_t {
            CLAMP_ENC     = 0x0001,   // pulses per detent, hold time
            CLAMP_POT     = 0x0002,
            CLAMP_ORDER   = 0x0004,
            CLAMP_LEVELS  = 0x0008,   // level steps, animation period
            CLAMP_BIND    = 0x0010,   // a binding to an encoder phase or no action
            CLAMP_ZONE    = 0x0020,   // react, idle animation, LED count
            CLAMP_FEET    = 0x0040,
            CLAMP_CURRENT = 0x0080,
        };
        // A CLAMP_* bit's name for the page (ASCII id), "" for none.
        static const char* clampName(uint16_t bit);

        // Flat NVS blob, versioned; another layout is refused whole.
        // v4: the second encoder. A v3 blob is still read: its encoder lands in
        // enc[ENC_RIGHT] and the left comes up disabled.
        // v5: the same layout; a v3/v4 blob whose right hold is unbound gets
        // Action::Menu there (the panel menu).
        static constexpr uint8_t VERSION   = 5;
        static constexpr uint8_t BLOB_SIZE = 116;
        static constexpr uint8_t BLOB_SIZE_V3 = 103;
        void toBlob(uint8_t* out) const;
        bool fromBlob(const uint8_t* in, uint8_t len);
    };

    const char* potModeName(PotMode m);
    PotMode     potModeFromName(const char* s);
    // The names the API stores and /api/catalog serves; row i = enum value i.
    // Sized here, so a row missing from the .cpp is a compile error.
    extern const UiName REACT_NAMES[(size_t)React::_Count];
    extern const UiName ANIM_NAMES[(size_t)Anim::_Count];
    extern const UiName ORDER_NAMES[(size_t)Order::_Count];
    // The knobs as /api/catalog panelKeys, in enc[] order.
    extern const UiName KNOB_KEYS[ENCODERS];

    const char* reactName(React r);
    React       reactFromName(const char* s);
    const char* animName(Anim a);
    Anim        animFromName(const char* s);
    const char* orderName(Order o);
    Order       orderFromName(const char* s);
}
