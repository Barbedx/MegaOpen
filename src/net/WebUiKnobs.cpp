#include "WebUi.h"
#if FEATURE_KNOBS
#include "../board/BoardProfile.h"
#include "../utils/LogBus.h"
#include <ArduinoJson.h>

// ---- /api/knobs: the two front-panel knobs and their LEDs ----
// Config document built once; live document polled (raw pot, encoder errors).
// POST applies first and saves only unless "save": false — a colour picker
// drag would otherwise burn NVS write cycles.

using namespace Knobs;

static void knobRgbOut(JsonObject o, const char* key, const Rgb& c)
{
    char b[8];
    snprintf(b, sizeof(b), "#%02x%02x%02x", c.r, c.g, c.b);
    o[key] = b;
}

static int knobNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "#rrggbb" only; anything else leaves the colour alone.
static void knobRgbIn(JsonVariantConst v, Rgb& c)
{
    const char* s = v.as<const char*>();
    if (!s || s[0] != '#' || strlen(s) != 7) return;
    int n[6];
    for (uint8_t i = 0; i < 6; ++i) { n[i] = knobNibble(s[i + 1]); if (n[i] < 0) return; }
    c.r = (uint8_t)((n[0] << 4) | n[1]);
    c.g = (uint8_t)((n[2] << 4) | n[3]);
    c.b = (uint8_t)((n[4] << 4) | n[5]);
}

static void knobBindOut(JsonObject parent, const char* key, const Binding& b)
{
    JsonObject o = parent[key].to<JsonObject>();
    o["action"] = Keys::name(b.action);
    o["code"]   = b.code;
}

static void knobBindIn(JsonVariantConst v, Binding& b)
{
    if (v["action"].is<const char*>()) b.action = Keys::fromName(v["action"].as<const char*>());
    if (v["code"].is<int>())           b.code   = (uint8_t)v["code"].as<int>();
}

void WebUi::routesKnobs()
{
    // ---- the whole configuration -------------------------------------------
    route("/api/knobs", HTTP_GET, [this](PsychicRequest* r) {
        if (!_knobs) return r->reply(503, "text/plain", "no knob service");
        const Config c = _knobs->config();
        JsonDocument d;

        d["board"] = Board::NAME;
        JsonObject pins = d["pins"].to<JsonObject>();
        pins["s1"]  = Board::PIN_KNOB_S1;
        pins["s2"]  = Board::PIN_KNOB_S2;
        pins["key"] = Board::PIN_KNOB_KEY;
        pins["pot"] = Board::PIN_KNOB_POT;
        pins["led"] = Board::PIN_KNOB_LED;
        // Per pin: false = no internal pull-up, relies on the encoder module's.
        JsonObject pu = d["pullup"].to<JsonObject>();
        pu["s1"]   = Board::hasPullup(Board::PIN_KNOB_S1);
        pu["s2"]   = Board::hasPullup(Board::PIN_KNOB_S2);
        pu["key"]  = Board::hasPullup(Board::PIN_KNOB_KEY);
        pu["s1b"]  = Board::PIN_KNOB2_S1  >= 0 && Board::hasPullup(Board::PIN_KNOB2_S1);
        pu["s2b"]  = Board::PIN_KNOB2_S2  >= 0 && Board::hasPullup(Board::PIN_KNOB2_S2);
        pu["keyb"] = Board::PIN_KNOB2_KEY >= 0 && Board::hasPullup(Board::PIN_KNOB2_KEY);
        d["maxLeds"] = MAX_LEDS;

        // `enc` = right knob, `enc2` = left; objects, not an array, for API compat.
        static const char* const kEncKey[ENCODERS] = {"enc", "enc2"};
        for (uint8_t k = 0; k < ENCODERS; ++k) {
            const Enc& en = c.enc[k];
            JsonObject e = d[kEncKey[k]].to<JsonObject>();
            e["fitted"]   = (k == ENC_RIGHT) || Board::hasKnob2();
            e["enabled"]  = en.enabled;
            e["inverted"] = en.inverted;
            e["ppd"]      = en.ppd;
            e["holdMs"]   = en.holdMs;
            knobBindOut(e, "cw", en.cw);
            knobBindOut(e, "ccw", en.ccw);
            knobBindOut(e, "click", en.click);
            knobBindOut(e, "hold", en.hold);
        }
        pins["s1b"]  = Board::PIN_KNOB2_S1;
        pins["s2b"]  = Board::PIN_KNOB2_S2;
        pins["keyb"] = Board::PIN_KNOB2_KEY;

        JsonObject p = d["pot"].to<JsonObject>();
        p["mode"]     = potModeName(c.potMode);
        p["inverted"] = c.potInverted;
        p["zones"]    = c.potZones;
        p["min"]      = c.potMin;
        p["max"]      = c.potMax;
        knobBindOut(p, "up", c.potUp);
        knobBindOut(p, "down", c.potDown);

        JsonObject l = d["led"].to<JsonObject>();
        l["enabled"]    = c.ledEnabled;
        l["order"]      = orderName(c.order);
        l["brightness"] = c.brightness;
        l["holdMs"]     = c.reactHoldMs;
        l["levelSteps"] = c.levelSteps;
        l["animPeriod"] = c.animPeriod;
        l["rightFirst"] = c.rightFirst;
        l["ringReverse"] = c.ringReverse;
        l["zoneLeds"]   = ZONE_LEDS;
        JsonArray zs = l["zones"].to<JsonArray>();
        for (uint8_t z = 0; z < ZONES; ++z) {
            JsonObject o = zs.add<JsonObject>();
            o["count"] = c.zone[z].count;
            o["react"] = reactName(c.zone[z].react);
            o["anim"]  = animName(c.zone[z].anim);
            // All eight, whatever `count` says, so hidden ones survive a save.
            JsonArray ls = o["leds"].to<JsonArray>();
            for (uint8_t i = 0; i < ZONE_LEDS; ++i) {
                char b[8];
                snprintf(b, sizeof(b), "#%02x%02x%02x",
                         c.zone[z].led[i].r, c.zone[z].led[i].g, c.zone[z].led[i].b);
                ls.add(b);
            }
            knobRgbOut(o, "lo", c.zone[z].lo);
            knobRgbOut(o, "hi", c.zone[z].hi);
        }

        JsonObject f = l["feet"].to<JsonObject>();
        f["count"]      = c.feetCount;
        f["max"]        = FEET_LEDS;
        f["anim"]       = animName(c.feetAnim);
        f["brightness"] = c.feetBrightness;
        knobRgbOut(f, "color", c.feetColor);
        l["maxMilliamps"] = c.maxMilliamps;

        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // ---- what the knobs are doing, polled ----------------------------------
    route("/api/knobs/live", HTTP_GET, [this](PsychicRequest* r) {
        if (!_knobs) return r->reply(503, "text/plain", "no knob service");
        const Service::Live v = _knobs->live();
        JsonDocument d;
        JsonObject e = d["enc"].to<JsonObject>();
        JsonObject e2 = d["enc2"].to<JsonObject>();
        e2["detents"] = v.enc2Detents; e2["pos"] = v.pos[1]; e["pos"] = v.pos[0]; e2["errors"] = v.enc2Errors;
        e2["btn"] = v.btn2; e2["clicks"] = v.clicks2; e2["holds"] = v.holds2;
        e["detents"] = v.encDetents;
        // A few while spinning = late samples. A steady climb at rest = floating
        // input (module pull-ups not reaching the pin).
        e["errors"]  = v.encErrors;
        e["btn"]     = v.btn;
        e["clicks"]  = v.clicks;
        e["holds"]   = v.holds;
        // Raw contacts, undecoded: what the wiring does. 1 = closed to ground.
        e["a"] = (v.lines & 1) != 0;  e["b"] = (v.lines & 2) != 0;  e["key"] = (v.lines & 4) != 0;
        e2["a"] = (v.lines & 8) != 0; e2["b"] = (v.lines & 16) != 0; e2["key"] = (v.lines & 32) != 0;
        // Per-line change counts (a, b, key): wiring a contact hunt watches.
        // A healthy turn moves a and b alike, two each per detent.
        JsonArray g = e["edges"].to<JsonArray>(), g2 = e2["edges"].to<JsonArray>();
        for (uint8_t i = 0; i < 3; ++i) { g.add(v.edges[i]); g2.add(v.edges[3 + i]); }
        // Knobs behind a PCF8574 (T-2CAN): addr 0 = not found; errors climbing
        // = loose QWIIC cable.
        if (Board::KNOBS_I2C) {
            JsonObject x = d["expander"].to<JsonObject>();
            x["addr"]   = v.expAddr;
            x["errors"] = v.expErrors;
        }
        JsonObject p = d["pot"].to<JsonObject>();
        p["raw"]   = v.potRaw;
        p["level"] = v.potLevel;
        p["zone"]  = v.potZone;
        p["steps"] = v.potSteps;
        JsonObject l = d["led"].to<JsonObject>();
        l["ok"]     = v.stripOk;
        l["frames"] = v.frames;
        l["master"] = v.master;
        l["leds"]   = v.leds;
        // mAWanted > mA means the current limiter is dimming the chain.
        l["mA"]       = v.mA;
        l["mAWanted"] = v.mAWanted;
        // Frames refused while the last was on the wire; 0 when healthy
        // (full chain 5 ms, frames 20 ms apart).
        l["busy"]     = v.busy;
        // Events the KeyBus refused (lane full): the keys task, which drains
        // it, stalled. Not the panel/loop task.
        d["dropped"] = v.dropped;
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // ---- settings ------------------------------------------------------------
    route("/api/knobs", HTTP_POST, [this](PsychicRequest* r) {
        if (!_knobs) return r->reply(503, "text/plain", "no knob service");
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad json");

        Config c = _knobs->config();

        static const char* const kEncKey[ENCODERS] = {"enc", "enc2"};
        for (uint8_t k = 0; k < ENCODERS; ++k) {
            JsonVariantConst e = d[kEncKey[k]];
            if (e.isNull()) continue;
            Enc& en = c.enc[k];
            if (e["enabled"].is<bool>())  en.enabled  = e["enabled"].as<bool>();
            if (e["inverted"].is<bool>()) en.inverted = e["inverted"].as<bool>();
            if (e["ppd"].is<int>())       en.ppd      = (uint8_t)e["ppd"].as<int>();
            if (e["holdMs"].is<int>())    en.holdMs   = (uint16_t)e["holdMs"].as<int>();
            knobBindIn(e["cw"], en.cw);
            knobBindIn(e["ccw"], en.ccw);
            knobBindIn(e["click"], en.click);
            knobBindIn(e["hold"], en.hold);
        }

        JsonVariantConst p = d["pot"];
        if (p["mode"].is<const char*>()) c.potMode     = potModeFromName(p["mode"].as<const char*>());
        if (p["inverted"].is<bool>())    c.potInverted = p["inverted"].as<bool>();
        if (p["zones"].is<int>())        c.potZones    = (uint8_t)p["zones"].as<int>();
        if (p["min"].is<int>())          c.potMin      = (uint16_t)p["min"].as<int>();
        if (p["max"].is<int>())          c.potMax      = (uint16_t)p["max"].as<int>();
        knobBindIn(p["up"], c.potUp);
        knobBindIn(p["down"], c.potDown);

        JsonVariantConst l = d["led"];
        if (l["enabled"].is<bool>())      c.ledEnabled  = l["enabled"].as<bool>();
        if (l["order"].is<const char*>()) c.order       = orderFromName(l["order"].as<const char*>());
        if (l["brightness"].is<int>())    c.brightness  = (uint8_t)l["brightness"].as<int>();
        if (l["holdMs"].is<int>())        c.reactHoldMs = (uint16_t)l["holdMs"].as<int>();
        if (l["levelSteps"].is<int>())    c.levelSteps  = (uint8_t)l["levelSteps"].as<int>();
        if (l["animPeriod"].is<int>())    c.animPeriod  = (uint8_t)l["animPeriod"].as<int>();
        if (l["rightFirst"].is<bool>())   c.rightFirst  = l["rightFirst"].as<bool>();
        if (l["ringReverse"].is<bool>())  c.ringReverse = l["ringReverse"].as<bool>();
        for (uint8_t z = 0; z < ZONES; ++z) {
            JsonVariantConst o = l["zones"][z];
            if (o.isNull()) continue;
            if (o["count"].is<int>())         c.zone[z].count = (uint8_t)o["count"].as<int>();
            if (o["react"].is<const char*>()) c.zone[z].react = reactFromName(o["react"].as<const char*>());
            if (o["anim"].is<const char*>())  c.zone[z].anim  = animFromName(o["anim"].as<const char*>());
            // A short array leaves the rest alone (knobRgbIn ignores null).
            for (uint8_t i = 0; i < ZONE_LEDS; ++i) knobRgbIn(o["leds"][i], c.zone[z].led[i]);
            knobRgbIn(o["lo"], c.zone[z].lo);
            knobRgbIn(o["hi"], c.zone[z].hi);
        }

        JsonVariantConst f = l["feet"];
        if (f["count"].is<int>())        c.feetCount      = (uint8_t)constrain(f["count"].as<int>(), 0, (int)FEET_LEDS);
        if (f["anim"].is<const char*>()) c.feetAnim       = animFromName(f["anim"].as<const char*>());
        if (f["brightness"].is<int>())   c.feetBrightness = (uint8_t)f["brightness"].as<int>();
        knobRgbIn(f["color"], c.feetColor);
        if (l["maxMilliamps"].is<int>())
            c.maxMilliamps = (uint16_t)constrain(l["maxMilliamps"].as<int>(), 0, 20000);

        c.sanitize();   // limits enforced here, not in the page
        _knobs->setConfig(c);

        const bool save = d["save"] | true;
        if (save) {
            _cfg->saveKnobs(c);
            LogBus::printf("[knobs] saved: enc=%s/%s pot=%s led=%s x%u+%u feet, cap %u mA",
                           c.enc[ENC_RIGHT].enabled ? "on" : "off",
                           c.enc[ENC_LEFT].enabled ? "on" : "off", potModeName(c.potMode),
                           c.ledEnabled ? "on" : "off",
                           (unsigned)(c.zone[0].count + c.zone[1].count),
                           (unsigned)c.feetCount, (unsigned)c.maxMilliamps);
        }
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // ---- pot end-of-travel calibration, from the filtered live reading ----
    route("/api/knobs/cal", HTTP_POST, [this](PsychicRequest* r) {
        if (!_knobs) return r->reply(503, "text/plain", "no knob service");
        JsonDocument d;
        deserializeJson(d, r->body());
        const char* end = d["end"] | "";
        const bool isMin = strcmp(end, "min") == 0;
        if (!isMin && strcmp(end, "max") != 0) return r->reply(400, "text/plain", "end: min|max");

        Config c = _knobs->config();
        const uint16_t raw = _knobs->live().potRaw;
        // min is always the smaller reading; `inverted` decides which end is up.
        if (isMin) c.potMin = raw; else c.potMax = raw;
        if (c.potMin > c.potMax) { const uint16_t t = c.potMin; c.potMin = c.potMax; c.potMax = t; }
        // 409, else sanitize() would silently reset to the full range.
        if (c.potMax - c.potMin < 64)
            return r->reply(409, "text/plain", "ends too close — turn the knob to the other end first");
        _knobs->setConfig(c);

        // Save into the stored config, not the live one, which may hold unsaved colours.
        Config stored;
        _cfg->loadKnobs(stored);
        stored.potMin = c.potMin;
        stored.potMax = c.potMax;
        _cfg->saveKnobs(stored);

        JsonDocument o;
        o["ok"] = true; o["min"] = c.potMin; o["max"] = c.potMax;
        String out; serializeJson(o, out);
        return replyJson(r, out);
    });
}
#endif  // FEATURE_KNOBS
