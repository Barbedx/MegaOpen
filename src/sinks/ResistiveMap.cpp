#include "ResistiveMap.h"

namespace Sinks
{
    // Five E24 values, each ~2.2x the last. Handed out one per button
    // (chooseMasks prefers single-leg rungs), so their spacing is what matters.
    // Ceiling: Android head units read ~5 kΩ max as a keypress; above that is
    // an open line. (The 6.8k-22k forum ladders are Pioneer's; do not port.)
    // Floor: rungs need ~93 Ω separation to survive cabin thermal drift; the
    // smallest gap here is 240 Ω.
    // Editable on the settings page to match whatever is actually fitted.
    static constexpr uint16_t kDefaultLegs[ResistiveLadder::MAX_LEGS] =
        { 150, 390, 820, 1800, 3300 };

    void ResistiveMap::loadDefaults()
    {
        for (uint8_t c = 0; c < CHANNELS; ++c) {
            _lad[c].setLegCount(ResistiveLadder::MAX_LEGS);
            for (uint8_t i = 0; i < ResistiveLadder::MAX_LEGS; ++i)
                _lad[c].setLeg(i, kDefaultLegs[i]);
            // ESP32 output driver pulling low, ~30 Ω (20% of the 150 Ω rung), so
            // the printed ohms match what the head unit sees. MOSFETs: ~2.
            _lad[c].setSeriesOhms(30);
        }

        // Nothing assigned: the sink must not press buttons at boot on a ladder
        // that may not be fitted. Auto-assign on the settings page is explicit.
        for (uint8_t i = 0; i < ACTIONS; ++i) _slot[i] = Slot{};

        _t = Timing{};
        // Pulse caps at ~9 detents/s; a plain hold waits 0.5 s for repeat.
        // Hybrid pulses until the queue saturates.
        _spin = Spin::Hybrid;
        // Bare GPIO: OUTPUT LOW engages the leg, INPUT (Hi-Z) releases; never
        // driven HIGH. Safe because KEY1/KEY2 on this head unit idle at 2.8 V
        // (measured), so the Hi-Z pin's clamp never conducts. A 5 V unit would
        // lose ~1/4 of idle level through the clamp: use MOSFETs there (see
        // docs/wiring-direct.md).
        _activeHigh = false;
    }

    const ResistiveMap::Slot& ResistiveMap::slot(Keys::Action a) const
    {
        const uint8_t i = (uint8_t)a;
        return _slot[i < ACTIONS ? i : 0];
    }

    void ResistiveMap::setSlot(Keys::Action a, Slot s)
    {
        const uint8_t i = (uint8_t)a;
        if (i >= ACTIONS) return;
        // Encoder phases never produce a Release, so a rung on one could never
        // be let go.
        if (Keys::isEncoderPhase(a)) return;
        if (s.ch >= CHANNELS) s.ch = 0;
        s.mask     &= (uint8_t)((1u << ResistiveLadder::MAX_LEGS) - 1);
        s.longMask &= (uint8_t)((1u << ResistiveLadder::MAX_LEGS) - 1);
        // Long-only is allowed; long == short would only add delay.
        if (s.longMask && s.longMask == s.mask) s.longMask = 0;
        _slot[i] = s;
    }

    uint8_t ResistiveMap::usedMasks(uint8_t ch, uint8_t* out, Keys::Action* who) const
    {
        uint8_t n = 0;
        for (uint8_t i = 1; i < ACTIONS; ++i) {
            if (_slot[i].mask == 0 || _slot[i].ch != ch) continue;
            if (out) out[n] = _slot[i].mask;
            if (who) who[n] = (Keys::Action)i;
            ++n;
        }
        return n;
    }

    uint8_t ResistiveMap::autoAssign(uint8_t ch, const Keys::Action* actions, uint8_t n)
    {
        if (ch >= CHANNELS || !actions || !n) return 0;

        // Clear the channel first so a re-run gives the same spread as one run.
        for (uint8_t i = 0; i < ACTIONS; ++i)
            if (_slot[i].ch == ch) _slot[i].mask = 0;

        uint8_t want = 0;
        for (uint8_t i = 0; i < n; ++i)
            if (!Keys::isEncoderPhase(actions[i]) && actions[i] != Keys::Action::None) ++want;
        if (!want) return 0;

        uint8_t masks[1u << ResistiveLadder::MAX_LEGS];
        const uint8_t got = _lad[ch].chooseMasks(want, masks);

        uint8_t placed = 0;
        for (uint8_t i = 0; i < n && placed < got; ++i) {
            const Keys::Action a = actions[i];
            if (Keys::isEncoderPhase(a) || a == Keys::Action::None) continue;
            setSlot(a, Slot{ch, masks[placed]});
            ++placed;
        }
        return placed;
    }

    // ---- persistence ---------------------------------------------------------
    // Flat, little-endian, version in byte 0. Wrong length or version: caller
    // keeps defaults, never a half-applied ladder.
    static constexpr uint8_t kBlobVersion = 1;

    void ResistiveMap::toBlob(uint8_t* out) const
    {
        uint16_t p = 0;
        out[p++] = kBlobVersion;
        out[p++] = (uint8_t)(_activeHigh ? 0x01 : 0x00);
        out[p++] = (uint8_t)_spin;
        for (uint8_t c = 0; c < CHANNELS; ++c) out[p++] = _lad[c].legCount();
        for (uint8_t c = 0; c < CHANNELS; ++c)
            for (uint8_t i = 0; i < ResistiveLadder::MAX_LEGS; ++i) {
                const uint16_t v = _lad[c].leg(i);
                out[p++] = (uint8_t)(v & 0xFF);
                out[p++] = (uint8_t)(v >> 8);
            }
        for (uint8_t c = 0; c < CHANNELS; ++c) {
            const uint16_t v = _lad[c].seriesOhms();
            out[p++] = (uint8_t)(v & 0xFF);
            out[p++] = (uint8_t)(v >> 8);
        }
        auto put16 = [&](uint16_t v) { out[p++] = (uint8_t)(v & 0xFF); out[p++] = (uint8_t)(v >> 8); };
        put16(_t.pulseMs);  put16(_t.gapMs);
        put16(_t.minPulseMs); put16(_t.minGapMs);
        put16(_t.maxHoldMs);
        out[p++] = _t.holdAfter;
        out[p++] = 0;                       // pad, keeps holdMaxMs 16-bit aligned
        put16(_t.holdMaxMs);
        put16(_t.repeatStartMs);
        put16(_t.repeatMs);
        for (uint8_t i = 0; i < ACTIONS; ++i)
            out[p++] = (uint8_t)((_slot[i].ch ? 0x80 : 0x00) | (_slot[i].mask & 0x1F));
        for (uint8_t i = 0; i < ACTIONS; ++i)
            out[p++] = (uint8_t)(_slot[i].longMask & 0x1F);
    }

    bool ResistiveMap::fromBlob(const uint8_t* in, uint16_t len)
    {
        if (!in || len != BLOB_SIZE || in[0] != kBlobVersion) return false;

        uint16_t p = 1;
        _activeHigh = (in[p++] & 0x01) != 0;
        const uint8_t sp = in[p++];
        _spin = (sp <= (uint8_t)Spin::Hybrid) ? (Spin)sp : Spin::Hybrid;

        uint8_t counts[CHANNELS];
        for (uint8_t c = 0; c < CHANNELS; ++c) counts[c] = in[p++];
        for (uint8_t c = 0; c < CHANNELS; ++c) {
            _lad[c].setLegCount(counts[c]);
            for (uint8_t i = 0; i < ResistiveLadder::MAX_LEGS; ++i) {
                const uint16_t v = (uint16_t)(in[p] | ((uint16_t)in[p + 1] << 8));
                p += 2;
                _lad[c].setLeg(i, v);
            }
        }
        for (uint8_t c = 0; c < CHANNELS; ++c) {
            _lad[c].setSeriesOhms((uint16_t)(in[p] | ((uint16_t)in[p + 1] << 8)));
            p += 2;
        }
        auto get16 = [&]() -> uint16_t {
            const uint16_t v = (uint16_t)(in[p] | ((uint16_t)in[p + 1] << 8));
            p += 2; return v;
        };
        _t.pulseMs    = get16();
        _t.gapMs      = get16();
        _t.minPulseMs = get16();
        _t.minGapMs   = get16();
        _t.maxHoldMs  = get16();
        _t.holdAfter  = in[p++];
        ++p;                                // pad
        _t.holdMaxMs  = get16();
        _t.repeatStartMs = get16();
        _t.repeatMs      = get16();

        // Zero maxHoldMs would disable the stuck-key guard; zero repeatMs would
        // make a Hybrid hold deliver nothing. Repair both.
        if (!_t.maxHoldMs) _t.maxHoldMs = Timing{}.maxHoldMs;
        if (!_t.pulseMs)   _t.pulseMs   = Timing{}.pulseMs;
        if (!_t.repeatMs)  _t.repeatMs  = Timing{}.repeatMs;

        for (uint8_t i = 0; i < ACTIONS; ++i) {
            const uint8_t b = in[p++];
            _slot[i].ch   = (b & 0x80) ? 1 : 0;
            _slot[i].mask = (uint8_t)(b & 0x1F);
        }
        for (uint8_t i = 0; i < ACTIONS; ++i)
            _slot[i].longMask = (uint8_t)(in[p++] & 0x1F);

        // Same rules as setSlot(); older blobs may violate them. Detents have
        // no hold, so encoder long rungs are cleared too.
        for (uint8_t i = 0; i < ACTIONS; ++i) {
            if (Keys::isEncoderPhase((Keys::Action)i)) { _slot[i].mask = 0; _slot[i].longMask = 0; }
            if (Keys::isEncoder((Keys::Action)i)) _slot[i].longMask = 0;
            if (_slot[i].longMask && _slot[i].longMask == _slot[i].mask) _slot[i].longMask = 0;
        }
        return true;
    }
}
