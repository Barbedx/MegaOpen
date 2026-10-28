#include "WebUi.h"
#include "../utils/LogBus.h"
#include "../app/KeyBus.h"
#if FEATURE_RESISTIVE_KEYS
#include "../board/BoardProfile.h"
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// /api/res — the KEY1/KEY2 resistor ladder (no page since ui-v2 slice 8)
// ---------------------------------------------------------------------------
// Own translation unit so one guard removes it: the shipping build turns
// FEATURE_RESISTIVE_KEYS off. Config and live state are separate documents;
// the page polls live twice a second and must not rebuild the whole ladder.

// The buttons the keypad actually has (from the key map, not the whole Action
// enum), in the order they should get the best rungs. The map is the staged
// copy: the scan task replaces the live one whole, and a read in the middle
// of that copy could be torn.
uint8_t WebUi::resWantedActions(Keys::Action* out, uint8_t cap) const
{
    uint8_t n = 0;
    App::KeysTask::KeyMapCfg k;
    const bool haveMap = _st.keymap && _st.keymap->peek(k);
    bool seen[(uint8_t)Keys::Action::_Count] = {};

    auto add = [&](Keys::Action a) {
        if (n >= cap || a == Keys::Action::None) return;
        if (Keys::isEncoderPhase(a)) return;
        if (seen[(uint8_t)a]) return;
        seen[(uint8_t)a] = true;
        out[n++] = a;
    };

    // Volume first: if the ladder runs short, a lesser key gets the bunched rung.
    add(Keys::Action::VolUp);
    add(Keys::Action::VolDown);
    if (haveMap)
        for (uint8_t p = 0; p < Keys::KeyMap::POSITIONS; ++p) add(k.map.slot(p).action);

    // The wheel, if phases are mapped; it has no position of its own.
    if (haveMap) {
        uint8_t pa, pb, pc;
        k.map.phases(pa, pb, pc);
        if (pa != 0xFF || pb != 0xFF || pc != 0xFF) {
            add(Keys::Action::EncCw);
            add(Keys::Action::EncCcw);
        }
    }
    return n;
}

// Everything here only reads the sink's live state. The ladder's map is staged
// (WebUi::Staging::resMap) and the learn hold is a request: the keys task owns
// the sink and drives the rung pins, and httpd runs on the other core.
void WebUi::routesRes()
{
    // ---- the whole configuration ------------------------------------------
    route("/api/res", HTTP_GET, [this](PsychicRequest* r) {
        if (!_res || !_st.resMap) return r->reply(503, "text/plain", "not_built resistive_sink");
        // The newest edit, applied or not: what the page just saved.
        Sinks::ResistiveMap m;
        if (!_st.resMap->peek(m)) return r->reply(503, "text/plain", "busy");
        JsonDocument d;
        d["enabled"]    = _res->enabled();
        d["activeHigh"] = m.activeHigh();
        d["spin"]       = (int)m.spin();
        d["classes"]    = _res->classes();

        const Sinks::ResistiveMap::Timing& t = m.timing();
        JsonObject to = d["timing"].to<JsonObject>();
        to["pulseMs"]       = t.pulseMs;
        to["gapMs"]         = t.gapMs;
        to["maxHoldMs"]     = t.maxHoldMs;
        to["holdAfter"]     = t.holdAfter;
        to["repeatStartMs"] = t.repeatStartMs;
        to["repeatMs"]      = t.repeatMs;

        JsonArray chs = d["ch"].to<JsonArray>();
        for (uint8_t c = 0; c < Sinks::ResistiveMap::CHANNELS; ++c) {
            const Sinks::ResistiveLadder& L = m.ladder(c);
            const uint8_t legs = Board::resLegs(c);
            JsonObject o = chs.add<JsonObject>();
            o["legs"]   = legs;
            o["series"] = L.seriesOhms();
            // Per channel: one leg outside the window poisons every rung it is in.
            o["inWindow"] = L.inUsableWindow();
            JsonArray lg = o["leg"].to<JsonArray>();
            JsonArray pn = o["pins"].to<JsonArray>();
            for (uint8_t i = 0; i < Sinks::ResistiveLadder::MAX_LEGS; ++i) lg.add(L.leg(i));
            for (uint8_t i = 0; i < legs; ++i) pn.add(Board::resPins(c)[i]);

            // Every reachable rung with its ohms, for the page's dropdown.
            JsonArray rg = o["rungs"].to<JsonArray>();
            for (uint8_t k = 1; k <= L.levelCount(); ++k) {
                const uint16_t ohms = L.ohmsFor(k);
                if (ohms == Sinks::ResistiveLadder::OPEN) continue;
                JsonObject ro = rg.add<JsonObject>();
                ro["mask"] = k;
                ro["ohms"] = ohms;
            }
        }

        Keys::Action want[Sinks::ResistiveMap::ACTIONS];
        const uint8_t nWant = resWantedActions(want, Sinks::ResistiveMap::ACTIONS);

        JsonArray acts = d["actions"].to<JsonArray>();
        for (uint8_t i = 0; i < nWant; ++i) {
            const Sinks::ResistiveMap::Slot s = m.slot(want[i]);
            JsonObject o = acts.add<JsonObject>();
            o["name"]     = Keys::name(want[i]);
            o["ch"]       = s.ch;
            o["mask"]     = s.mask;
            o["longMask"] = s.longMask;
            o["ohms"]     = m.ladder(s.ch).ohmsFor(s.mask);
            // A detent has no hold, so the page must not offer it a long rung.
            o["enc"]      = Keys::isEncoder(want[i]);
        }

        // Tightest separation among assigned rungs only; spare rungs cannot collide.
        uint8_t worst = 255;
        for (uint8_t c = 0; c < Sinks::ResistiveMap::CHANNELS; ++c) {
            uint8_t used[64];
            uint8_t nUsed = 0;
            for (uint8_t i = 0; i < nWant && nUsed < 62; ++i) {
                const Sinks::ResistiveMap::Slot s = m.slot(want[i]);
                if (s.ch != c) continue;
                if (s.mask)     used[nUsed++] = s.mask;
                if (s.longMask) used[nUsed++] = s.longMask;
            }
            if (nUsed < 2) continue;
            const uint8_t w = m.ladder(c).worstSeparationPct(used, nUsed);
            if (w < worst) worst = w;
        }
        d["sep"] = worst;

        // ---- the spare rungs -------------------------------------------------
        // Rungs no action uses: extra buttons that can be taught to the head unit.
        JsonArray freeRungs = d["free"].to<JsonArray>();
        for (uint8_t c = 0; c < Sinks::ResistiveMap::CHANNELS; ++c) {
            const Sinks::ResistiveLadder& L = m.ladder(c);
            for (uint8_t k = 1; k <= L.levelCount(); ++k) {
                const uint16_t ohms = L.ohmsFor(k);
                if (ohms == Sinks::ResistiveLadder::OPEN) continue;
                bool taken = false;
                for (uint8_t i = 0; i < Sinks::ResistiveMap::ACTIONS && !taken; ++i) {
                    const Sinks::ResistiveMap::Slot s = m.slot((Keys::Action)i);
                    if (s.ch == c && (s.mask == k || s.longMask == k)) taken = true;
                }
                if (taken) continue;
                JsonObject o = freeRungs.add<JsonObject>();
                o["ch"] = c; o["mask"] = k; o["ohms"] = ohms;
            }
        }

        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // ---- what is on the wire, polled --------------------------------------
    route("/api/res/live", HTTP_GET, [this](PsychicRequest* r) {
        if (!_res || !_st.resMap) return r->reply(503, "text/plain", "not_built resistive_sink");
        // Ohms from the staged map, not liveOhms(): that reads the keys task's
        // own copy, which it may be replacing right now.
        Sinks::ResistiveMap m;
        if (!_st.resMap->peek(m)) return r->reply(503, "text/plain", "busy");
        JsonDocument d;
        JsonArray chs = d["ch"].to<JsonArray>();
        for (uint8_t c = 0; c < Sinks::ResistiveMap::CHANNELS; ++c) {
            JsonObject o = chs.add<JsonObject>();
            const uint8_t mask = _res->liveMask(c);
            o["mask"]  = mask;
            o["ohms"]  = m.ladder(c).ohmsFor(mask);
            o["queue"] = _res->queued(c);
            o["learn"] = _res->learning(c);
        }
        d["presses"]  = _res->presses();
        d["detents"]  = _res->detents();
        d["dropped"]  = _res->dropped();
        d["preempts"] = _res->preempts();
        d["longs"]    = _res->longs();
        // Non-zero = a Release never arrived (fault upstream of the sink).
        d["stuck"]    = _res->stuck();
        String out; serializeJson(d, out);
        return replyJson(r, out);
    });

    // ---- settings ----------------------------------------------------------
    // Every map edit below is staged; the keys task swaps it in and releases
    // every rung as it does. The copy saved to NVS is taken out of the edit,
    // so the flash write never runs under the staging mutex.
    route("/api/res", HTTP_POST, [this](PsychicRequest* r) {
        if (!_res || !_st.resMap) return r->reply(503, "text/plain", "not_built resistive_sink");
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");

        Sinks::ResistiveMap saved;
        Sinks::ResistiveMap::Timing t;
        const bool ok = _st.resMap->edit([&](Sinks::ResistiveMap& m) {
            if (d["activeHigh"].is<bool>()) m.setActiveHigh(d["activeHigh"].as<bool>());
            if (d["spin"].is<int>()) {
                const int s = d["spin"].as<int>();
                if (s >= 0 && s <= (int)Sinks::ResistiveMap::Spin::Hybrid)
                    m.setSpin((Sinks::ResistiveMap::Spin)s);
            }
            for (uint8_t c = 0; c < Sinks::ResistiveMap::CHANNELS; ++c) {
                if (d["legs"][c].is<JsonArray>())
                    for (uint8_t i = 0; i < Sinks::ResistiveLadder::MAX_LEGS; ++i)
                        if (d["legs"][c][i].is<int>())
                            m.ladder(c).setLeg(i, (uint16_t)d["legs"][c][i].as<int>());
                if (d["series"][c].is<int>())
                    m.ladder(c).setSeriesOhms((uint16_t)d["series"][c].as<int>());
            }

            t = m.timing();
            JsonObject ti = d["timing"].as<JsonObject>();
            if (!ti.isNull()) {
                if (ti["pulseMs"].is<int>())       t.pulseMs       = (uint16_t)ti["pulseMs"].as<int>();
                if (ti["gapMs"].is<int>())         t.gapMs         = (uint16_t)ti["gapMs"].as<int>();
                if (ti["maxHoldMs"].is<int>())     t.maxHoldMs     = (uint16_t)ti["maxHoldMs"].as<int>();
                if (ti["holdAfter"].is<int>())     t.holdAfter     = (uint8_t) ti["holdAfter"].as<int>();
                if (ti["repeatStartMs"].is<int>()) t.repeatStartMs = (uint16_t)ti["repeatStartMs"].as<int>();
                if (ti["repeatMs"].is<int>())      t.repeatMs      = (uint16_t)ti["repeatMs"].as<int>();
            }
            // Clamped server-side too: below 50 ms a real head unit was measured to
            // stop recognising presses entirely.
            if (t.pulseMs    < 50) t.pulseMs    = 50;
            if (t.minPulseMs < 50) t.minPulseMs = 50;
            if (t.gapMs      < 20) t.gapMs      = 20;
            if (!t.maxHoldMs)      t.maxHoldMs  = Sinks::ResistiveMap::Timing{}.maxHoldMs;
            if (!t.repeatMs)       t.repeatMs   = Sinks::ResistiveMap::Timing{}.repeatMs;
            m.timing() = t;
            saved = m;
        });
        if (!ok) return r->reply(503, "text/plain", "busy");

        if (d["enabled"].is<bool>()) {
            _cfg->s().resEnabled = d["enabled"].as<bool>();
            // Staged for the keys task, which drives the ladder; setEnabled(false)
            // there also releases a stuck rung.
            const bool on = _cfg->s().resEnabled;
            if (_st.routing) _st.routing->edit([&](App::KeysTask::Routing& k) { k.resOn = on; });
        }
        App::KeyBus::kick();
        _cfg->saveResMap(saved);
        _cfg->save();
        LOGI("res", "%s drive=%s spin=%u pulse=%u gap=%u",
                    _cfg->s().resEnabled ? "on" : "off",
                    saved.activeHigh() ? "mosfet" : "gpio",
                    (unsigned)saved.spin(), (unsigned)t.pulseMs, (unsigned)t.gapMs);
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // ---- one action's rungs -------------------------------------------------
    route("/api/res/map", HTTP_POST, [this](PsychicRequest* r) {
        if (!_res || !_st.resMap) return r->reply(503, "text/plain", "not_built resistive_sink");
        JsonDocument d;
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        const Keys::Action a = Keys::fromName(d["action"] | "");
        if (a == Keys::Action::None) return r->reply(400, "text/plain", "param action");

        // Remapping under a held key would strand the old rung (its Release
        // resolves against the new one): the keys task lets go of every rung
        // when it applies this.
        Sinks::ResistiveMap saved;
        const bool ok = _st.resMap->edit([&](Sinks::ResistiveMap& m) {
            Sinks::ResistiveMap::Slot s = m.slot(a);
            if (d["ch"].is<int>())       s.ch       = (uint8_t)d["ch"].as<int>();
            if (d["mask"].is<int>())     s.mask     = (uint8_t)d["mask"].as<int>();
            if (d["longMask"].is<int>()) s.longMask = (uint8_t)d["longMask"].as<int>();
            m.setSlot(a, s);
            saved = m;
        });
        if (!ok) return r->reply(503, "text/plain", "busy");
        App::KeyBus::kick();
        _cfg->saveResMap(saved);
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // ---- give every button a rung -------------------------------------------
    route("/api/res/auto", HTTP_POST, [this](PsychicRequest* r) {
        if (!_res || !_st.resMap) return r->reply(503, "text/plain", "not_built resistive_sink");
        JsonDocument d;
        deserializeJson(d, r->body());
        const uint8_t ch = (uint8_t)(d["ch"] | 0);

        Keys::Action want[Sinks::ResistiveMap::ACTIONS];
        const uint8_t n = resWantedActions(want, Sinks::ResistiveMap::ACTIONS);
        uint8_t placed = 0;
        Sinks::ResistiveMap saved;
        const bool ok = _st.resMap->edit([&](Sinks::ResistiveMap& m) {
            placed = m.autoAssign(ch, want, n);
            saved = m;
        });
        if (!ok) return r->reply(503, "text/plain", "busy");
        App::KeyBus::kick();
        _cfg->saveResMap(saved);

        JsonDocument o;
        o["ok"]     = true;
        o["wanted"] = n;
        // Fewer rungs than buttons is normal on a one-leg channel; report it.
        o["placed"] = placed;
        String out; serializeJson(o, out);
        LOGI("res", "auto-assign ch%u placed %u of %u",
                    (unsigned)ch, (unsigned)placed, (unsigned)n);
        return replyJson(r, out);
    });

    // ---- the learn hold -----------------------------------------------------
    // One endpoint: teach a mapped action, teach a spare rung, or (short `ms`)
    // fire a spare rung once. The rung is checked here against the staged map,
    // so the page still gets its 409; the hold itself is the keys task's.
    route("/api/res/learn", HTTP_POST, [this](PsychicRequest* r) {
        if (!_res || !_st.resMap || !_st.resLearn) return r->reply(503, "text/plain", "not_built resistive_sink");
        JsonDocument d;
        deserializeJson(d, r->body());

        if (d["stop"].as<bool>()) {
            // allOff() also ends a learn hold.
            if (_st.resAllOff) _st.resAllOff();
            return r->reply(200, "application/json", "{\"ok\":true}");
        }

        Sinks::ResistiveMap m;
        if (!_st.resMap->peek(m)) return r->reply(503, "text/plain", "busy");
        App::KeysTask::ResLearn q;
        q.ms = (uint32_t)(d["ms"] | (int)Sinks::ResistiveKeySink::LEARN_MS);

        if (d["action"].is<const char*>()) {
            const Keys::Action a = Keys::fromName(d["action"].as<const char*>());
            if (a == Keys::Action::None) return r->reply(400, "text/plain", "param action");
            // 409 with a reason: otherwise the press silently does nothing.
            // The same test learnAction() makes.
            const Sinks::ResistiveMap::Slot s = m.slot(a);
            if (!s.mask || m.ladder(s.ch).ohmsFor(s.mask) == Sinks::ResistiveLadder::OPEN)
                return r->reply(409, "text/plain", "res_no_legs");
            q.op = App::KeysTask::ResLearn::Op::Action;
            q.action = (uint8_t)a;
            _st.resLearn(q);
            LOGI("res", "holding %s for %u ms — choose the function on the "
                        "head unit FIRST, then press this", Keys::name(a), (unsigned)q.ms);
            return r->reply(200, "application/json", "{\"ok\":true}");
        }

        const uint8_t ch   = (uint8_t)(d["ch"] | 0);
        const uint8_t mask = (uint8_t)(d["mask"] | 0);
        if (!mask) return r->reply(400, "text/plain", "param mask");
        const uint16_t ohms = m.ladder(ch).ohmsFor(mask);
        if (ohms == Sinks::ResistiveLadder::OPEN)
            return r->reply(409, "text/plain", "res_no_legs");
        q.op = App::KeysTask::ResLearn::Op::Assert;
        q.ch = ch;
        q.mask = mask;
        _st.resLearn(q);
        LOGI("res", "holding ch%u mask 0x%02X (%u ohm) for %u ms",
                    (unsigned)ch, (unsigned)mask, (unsigned)ohms, (unsigned)q.ms);
        return r->reply(200, "application/json", "{\"ok\":true}");
    });
}
#endif  // FEATURE_RESISTIVE_KEYS
