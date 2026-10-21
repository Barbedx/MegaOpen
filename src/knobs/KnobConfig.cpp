#include "KnobConfig.h"
#include "../board/BoardProfile.h"
#include <string.h>

namespace Knobs
{
    Config::Config()
    {
        // Amber at rest (Megane instrument lighting); gradient cold to hot.
        for (uint8_t z = 0; z < ZONES; ++z) {
            for (uint8_t i = 0; i < ZONE_LEDS; ++i) zone[z].led[i] = Rgb{255, 80, 0};
            zone[z].lo = Rgb{0, 120, 255};
            zone[z].hi = Rgb{255, 20, 0};
        }
        feetColor = Rgb{255, 80, 0};

        // Bezel engravings: right = PAUSE / PREV / NEXT, left = VOL. Disabled
        // until enabled on /knobs.
        enc[ENC_RIGHT].cw    = Binding{Keys::Action::EncCw,   0};
        enc[ENC_RIGHT].ccw   = Binding{Keys::Action::EncCcw,  0};
        enc[ENC_RIGHT].click = Binding{Keys::Action::Ok,      0};
        // Holding the right knob opens the panel menu (App::PanelMenu).
        enc[ENC_RIGHT].hold  = Binding{Keys::Action::Menu,    0};
        enc[ENC_LEFT].cw    = Binding{Keys::Action::VolUp,   0};
        enc[ENC_LEFT].ccw    = Binding{Keys::Action::VolDown, 0};
        enc[ENC_LEFT].click  = Binding{Keys::Action::Mute,    0};

        // Behind a PCF8574 (T-2CAN) nothing floats: an open line reads high
        // through the expander's own pull-up, and with the expander unplugged
        // there are no samples at all. So both encoders come up ON there and a
        // fresh board's knobs work without a visit to /knobs. The LEDs stay OFF
        // everywhere: the chain's length and supply are the owner's to state.
        if (Board::KNOBS_I2C) {
            enc[ENC_RIGHT].enabled = true;
            if (Board::hasKnob2()) enc[ENC_LEFT].enabled = true;
        }
    }

    void Config::sanitize()
    {
        for (uint8_t k = 0; k < ENCODERS; ++k) {
            Enc& e = enc[k];
            if (e.ppd != 1 && e.ppd != 2) e.ppd = 4;
            if (e.holdMs < 200)  e.holdMs = 200;
            if (e.holdMs > 5000) e.holdMs = 5000;
        }

        if ((uint8_t)potMode >= (uint8_t)PotMode::_Count) potMode = PotMode::Off;
        if (potZones < 2)  potZones = 2;
        if (potZones > 64) potZones = 64;
        if (potMax > 4095) potMax = 4095;
        if (potMax <= potMin) { potMin = 0; potMax = 4095; }

        if ((uint8_t)order >= (uint8_t)Order::_Count) order = Order::GRB;
        if (levelSteps < 4) levelSteps = 4;
        if (animPeriod < 10) animPeriod = 10;           // a second, at the fastest

        // Never an encoder phase: those are matrix positions, not events.
        Binding* all[] = { &enc[0].cw, &enc[0].ccw, &enc[0].click, &enc[0].hold,
                           &enc[1].cw, &enc[1].ccw, &enc[1].click, &enc[1].hold,
                           &potUp, &potDown };
        for (Binding* b : all) {
            if ((uint8_t)b->action >= (uint8_t)Keys::Action::_Count ||
                Keys::isEncoderPhase(b->action)) *b = Binding{};
        }

        for (uint8_t z = 0; z < ZONES; ++z) {
            if ((uint8_t)zone[z].react >= (uint8_t)React::_Count) zone[z].react = React::None;
            if ((uint8_t)zone[z].anim  >= (uint8_t)Anim::_Count)  zone[z].anim  = Anim::Static;
            if (zone[z].count > ZONE_LEDS) zone[z].count = ZONE_LEDS;
        }

        if (feetCount > FEET_LEDS) feetCount = FEET_LEDS;
        if ((uint8_t)feetAnim >= (uint8_t)Anim::_Count) feetAnim = Anim::Static;
        // Floor: below it the limiter blacks out a merely idling chain.
        if (maxMilliamps < 200)   maxMilliamps = 200;
        if (maxMilliamps > 20000) maxMilliamps = 20000;
    }

    // ---- blob ---------------------------------------------------------------
    namespace {
        struct Writer {
            uint8_t* p; uint8_t i = 0;
            void u8(uint8_t v)   { p[i++] = v; }
            void u16(uint16_t v) { p[i++] = (uint8_t)(v & 0xFF); p[i++] = (uint8_t)(v >> 8); }
            void bind(const Binding& b) { u8((uint8_t)b.action); u8(b.code); }
            void rgb(const Rgb& c) { u8(c.r); u8(c.g); u8(c.b); }
        };
        struct Reader {
            const uint8_t* p; uint8_t i = 0;
            uint8_t  u8()  { return p[i++]; }
            uint16_t u16() { const uint16_t v = (uint16_t)(p[i] | (p[i + 1] << 8)); i += 2; return v; }
            Binding  bind() { Binding b; b.action = (Keys::Action)u8(); b.code = u8(); return b; }
            Rgb      rgb() { Rgb c; c.r = u8(); c.g = u8(); c.b = u8(); return c; }
        };
    }

    void Config::toBlob(uint8_t* out) const
    {
        memset(out, 0, BLOB_SIZE);
        Writer w{out};
        w.u8(VERSION);
        w.u8((uint8_t)((enc[0].enabled ? 0x01 : 0) | (enc[0].inverted ? 0x02 : 0) |
                       (potInverted ? 0x04 : 0) | (ledEnabled ? 0x08 : 0) |
                       (rightFirst ? 0x10 : 0) |
                       (enc[1].enabled ? 0x20 : 0) | (enc[1].inverted ? 0x40 : 0) |
                       (ringReverse ? 0x80 : 0)));
        w.u8(enc[0].ppd);
        w.bind(enc[0].cw); w.bind(enc[0].ccw); w.bind(enc[0].click); w.bind(enc[0].hold);
        w.u16(enc[0].holdMs);
        w.u8((uint8_t)potMode);
        w.u8(potZones);
        w.u16(potMin); w.u16(potMax);
        w.bind(potUp); w.bind(potDown);
        w.u8((uint8_t)order);
        w.u8(brightness);
        w.u16(reactHoldMs);
        w.u8(levelSteps);
        w.u8(animPeriod);
        for (uint8_t z = 0; z < ZONES; ++z) {
            w.u8(zone[z].count);
            w.u8((uint8_t)zone[z].react);
            w.u8((uint8_t)zone[z].anim);
            for (uint8_t i = 0; i < ZONE_LEDS; ++i) w.rgb(zone[z].led[i]);
            w.rgb(zone[z].lo); w.rgb(zone[z].hi);
        }
        w.u8(feetCount);
        w.u8((uint8_t)feetAnim);
        w.rgb(feetColor);
        w.u8(feetBrightness);
        w.u16(maxMilliamps);
        // v4: the LEFT encoder, appended so a v3 reader's offsets are untouched.
        w.u8(enc[1].ppd);
        w.bind(enc[1].cw); w.bind(enc[1].ccw); w.bind(enc[1].click); w.bind(enc[1].hold);
        w.u16(enc[1].holdMs);
        w.u8(0); w.u8(0);                        // spare
    }

    bool Config::fromBlob(const uint8_t* in, uint8_t len)
    {
        // v3 (one encoder, 103 bytes) is still accepted, and v4 (the v5 layout).
        const bool v3 = in && len == BLOB_SIZE_V3 && in[0] == 3;
        if (!in || (!v3 && (len != BLOB_SIZE || (in[0] != VERSION && in[0] != 4)))) return false;
        Reader r{in};
        Config c;
        r.u8();
        const uint8_t f = r.u8();
        c.enc[0].enabled  = f & 0x01; c.enc[0].inverted = f & 0x02;
        c.potInverted = f & 0x04; c.ledEnabled  = f & 0x08;
        c.rightFirst  = f & 0x10;
        c.enc[1].enabled  = f & 0x20; c.enc[1].inverted = f & 0x40;
        c.ringReverse = f & 0x80;
        c.enc[0].ppd = r.u8();
        c.enc[0].cw = r.bind(); c.enc[0].ccw = r.bind(); c.enc[0].click = r.bind(); c.enc[0].hold = r.bind();
        c.enc[0].holdMs = r.u16();
        c.potMode  = (PotMode)r.u8();
        c.potZones = r.u8();
        c.potMin = r.u16(); c.potMax = r.u16();
        c.potUp = r.bind(); c.potDown = r.bind();
        c.order = (Order)r.u8();
        c.brightness  = r.u8();
        c.reactHoldMs = r.u16();
        c.levelSteps  = r.u8();
        c.animPeriod  = r.u8();
        for (uint8_t z = 0; z < ZONES; ++z) {
            c.zone[z].count = r.u8();
            c.zone[z].react = (React)r.u8();
            c.zone[z].anim  = (Anim)r.u8();
            for (uint8_t i = 0; i < ZONE_LEDS; ++i) c.zone[z].led[i] = r.rgb();
            c.zone[z].lo = r.rgb(); c.zone[z].hi = r.rgb();
        }
        c.feetCount      = r.u8();
        c.feetAnim       = (Anim)r.u8();
        c.feetColor      = r.rgb();
        c.feetBrightness = r.u8();
        c.maxMilliamps   = r.u16();
        if (!v3) {
            c.enc[1].ppd = r.u8();
            c.enc[1].cw = r.bind(); c.enc[1].ccw = r.bind(); c.enc[1].click = r.bind(); c.enc[1].hold = r.bind();
            c.enc[1].holdMs = r.u16();
        }
        // Before v5 the right knob's hold had no job: it opens the menu now.
        // A hold someone bound on purpose is kept.
        if (in[0] < 5 && c.enc[ENC_RIGHT].hold.action == Keys::Action::None)
            c.enc[ENC_RIGHT].hold = Binding{Keys::Action::Menu, 0};
        c.sanitize();
        *this = c;
        return true;
    }

    // ---- names --------------------------------------------------------------
    static bool same(const char* a, const char* b) { return a && b && strcmp(a, b) == 0; }

    const char* potModeName(PotMode m) {
        switch (m) {
            case PotMode::Steps:      return "steps";
            case PotMode::Brightness: return "brightness";
            default:                  return "off";
        }
    }
    PotMode potModeFromName(const char* s) {
        if (same(s, "steps"))      return PotMode::Steps;
        if (same(s, "brightness")) return PotMode::Brightness;
        return PotMode::Off;
    }

    // Row i is the enum value i; row 0 is the fallback for a name nobody knows.
    const UiName REACT_NAMES[] = {
        {"none", "нема"}, {"level", "рівень"}, {"flash", "спалах"},
        {"spin", "обертання"}, {"sweep", "розмах"}, {"trail", "зі шлейфом"},
    };
    const UiName ANIM_NAMES[]  = {{"static", "статично"}, {"breathe", "дихання"}, {"rainbow", "веселка"}};
    const UiName ORDER_NAMES[] = {{"grb", "GRB"}, {"rgb", "RGB"}, {"grbw", "RGBW (SK6812)"}};
    const UiName KNOB_KEYS[]   = {{"KR", "Ручка П"}, {"KL", "Ручка Л"}};

    const char* reactName(React r)          { return uiId(REACT_NAMES, (uint8_t)r); }
    React       reactFromName(const char* s) { return (React)uiIndex(REACT_NAMES, s); }
    const char* animName(Anim a)            { return uiId(ANIM_NAMES, (uint8_t)a); }
    Anim        animFromName(const char* s)  { return (Anim)uiIndex(ANIM_NAMES, s); }
    const char* orderName(Order o)          { return uiId(ORDER_NAMES, (uint8_t)o); }
    Order       orderFromName(const char* s) { return (Order)uiIndex(ORDER_NAMES, s); }
}
