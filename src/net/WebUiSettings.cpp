#include "WebUi.h"
#include "../utils/HeapSentinel.h"
#include "../link/AppLink.h"
#include "RescuePage.h"
#include "lwip/sockets.h"
#include "../cfg/SettingsJson.h"
#include "../proto/canbox/Protocol.h"
#include "Catalog.h"
#include "../cfg/SettingsRegistry.h"
#include "../keys/KeyDests.h"
#include "GhOta.h"
#include "LogPush.h"
#include "../utils/AllocWatch.h"
#include <ArduinoJson.h>
#include <ElegantOTA.h>
#include <WiFi.h>
#include <driver/twai.h>
#include <soc/twai_struct.h>   // TWAI.error_code_capture_reg, for /api/can/ecc
#include <driver/gpio.h>
#include "../diag/CanBench.h"
#include "../board/BoardProfile.h"
#include "../display/PanelRole.h"
#include "../Version.h"
#include "../app/TaskPlan.h"
#include "../app/KeyBus.h"
#include "../app/HuTask.h"
#include "../app/CarTask.h"
#include "../utils/Log.h"
#include "../utils/LogBus.h"
#include "../utils/LogRing.h"
#if FEATURE_LOGFS
#include "../utils/LogFile.h"
#include <LittleFS.h>
#endif
#include <esp_system.h>
#include "../utils/Scratch.h"
#include "../utils/JsonEscape.h"
#include "../utils/Hex.h"
#if FEATURE_BLE && FEATURE_ANCS
#include "../apple_notification_service.h"
#endif

using namespace Keys;
#include "WebUiKit.h"

// ---- settings: the registry, the config file, the keypad map, WiFi ------------

// Every destination the catalog lists (Keys::DEST_IDS), from the staged
// routing: its mask, whether it can take a key at all (`live`, and `code` when
// it cannot: the page has the words, kb.why.<code>), and `state`. For the keys
// chain of /api/health.
void WebUi::putDests(JsonObject out, const App::KeysTask::Routing& k) const
{
    auto dest = [&](Keys::Dest i, uint8_t mask, bool live, const char* code) {
        JsonObject o = out[Keys::DEST_IDS[i]].to<JsonObject>();
        o["mask"]  = mask;
        o["live"]  = live;
        o["state"] = !live ? "off" : (mask ? "ok" : "muted");
        if (!live) o["code"] = code;
    };
    // Live only while the head unit is heard: keys to a silent one go nowhere.
    // A build that cannot tell (no HuMediaSource) counts it live.
#if FEATURE_DISPLAY_CAN
    const bool heard = !_hu || _hu->heard(millis());
#else
    const bool heard = true;
#endif
    dest(Keys::DEST_UART, k.uartCls, k.uartOn && heard,
         !k.uartOn ? "uart_off" : "hu_silent");
#if FEATURE_BLE
    // `volFallback`: volume goes to the phone because nothing live takes it
    // (Keys::volumeToAms); the mask is the user's, without the fallback bit.
    const bool vol = _st.volToAms && _st.volToAms();
    dest(Keys::DEST_AMS, k.amsCls, _amsSink != nullptr, "no_ams_role");
    out[Keys::DEST_IDS[Keys::DEST_AMS]]["volFallback"] = vol;
    dest(Keys::DEST_HID, k.hidCls, _hidSink != nullptr, "no_hid_role");
#else
    dest(Keys::DEST_AMS, 0, false, "not_built");
    dest(Keys::DEST_HID, 0, false, "not_built");
#endif
#if FEATURE_RESISTIVE_KEYS
    dest(Keys::DEST_RES, k.resCls, k.resOn, "res_off");   // KEY1/KEY2 ladder
#else
    dest(Keys::DEST_RES, 0, false, "not_built");
#endif
#if FEATURE_USB_HID
    // Always in the chain; whether a host is plugged in is not known here.
    dest(Keys::DEST_USB, k.usbCls, true, "");
#endif
}

// The registry's values as GET /api/settings sends them, or (`forFile`) as a
// settings file carries them: on copies of what each owner holds now.
void WebUi::settingsOut(JsonObject d, bool forFile)
{
    auto& s = _cfg->s();
    // The emitter's, likewise (the hu task owns it). Per request (Scratch):
    // ~450 B with the field map, neither on httpd's stack nor static.
    Scratch<Canbox::HuConfig> huBox;
    Canbox::HuConfig* hu = _emitter && huBox && App::Hu::emitterCfg().peek(*huBox) ? huBox.get() : nullptr;
#if FEATURE_CAPTURE
    Capture::Settings cs = Capture::settings();
    Capture::Settings* csp = &cs;
#else
    Capture::Settings* csp = nullptr;
#endif
#if FEATURE_KNOBS
    Knobs::Config kc;
    if (_knobs) kc = _knobs->config();
    const Cfg::Model m{&s, _knobs ? &kc : nullptr, csp,
                       hu ? hu->tuning : nullptr, hu ? hu->msgOn : nullptr};
#else
    const Cfg::Model m{&s, nullptr, csp, hu ? hu->tuning : nullptr, hu ? hu->msgOn : nullptr};
#endif
    Cfg::SettingsJson::write(m, d, forFile);
}

// POST /api/settings and the settings half of POST /api/config: check every
// key, then (unless `dry`) apply and hand each part to the task that owns it.
// `file`: a settings file, whose volatile entries are skipped. Returns the
// HTTP status; `out`/`type` are the body.
int WebUi::settingsPost(JsonObjectConst d, bool dry, bool file, String& out, const char*& type,
                        const Cfg::SettingsJson::Errors* extra)
{
        auto& s = _cfg->s();
        // The AP password, which is also the OTA password. Empty = unchanged.
        // Never from a file (SettingsJson::read refuses it there).
        const String apPass = file ? String() : String(d["apPass"] | "");
        if (apPass.length() && (apPass.length() < 8 || apPass.length() > 63))
            { out = "apPass: pass_len"; return 400; }

        // The emitter's settings are edited on a copy of what the hu task
        // holds now (cells not sent keep their values); the copy goes back
        // only when something in it changed. Per request (Scratch): ~450 B
        // with the field map.
        Scratch<Canbox::HuConfig> huBox;
        if (!huBox) { out = "no_memory"; return 503; }
        Canbox::HuConfig& hu = *huBox;
        const bool huOk = _emitter && App::Hu::emitterCfg().peek(hu);
        static uint8_t huWas[Canbox::HuConfig::TUNING_MAX];
        memcpy(huWas, hu.tuning, sizeof(huWas));
#if FEATURE_CAPTURE
        Capture::Settings cs = Capture::settings();
        Capture::Settings* csp = &cs;
#else
        Capture::Settings* csp = nullptr;
#endif
#if FEATURE_KNOBS
        // Read here, set whole at the end: safe because this handler holds the
        // settings lock throughout, as the menu's knob commits do (dispatch()).
        Knobs::Config kc;
        if (_knobs) kc = _knobs->config();
        Cfg::Model m{&s, _knobs ? &kc : nullptr, csp, huOk ? hu.tuning : nullptr, huOk ? hu.msgOn : nullptr};
#else
        Cfg::Model m{&s, nullptr, csp, huOk ? hu.tuning : nullptr, huOk ? hu.msgOn : nullptr};
#endif
        // A file skips what this board does not have, so a part that is
        // merely busy (its owner held it for the peek) must not pass for
        // missing: retry before anything is read or applied.
        if (file && _emitter && !huOk) { out = "busy"; return 503; }

        // Everything that can be refused is checked before anything is applied
        // (the /api/map rule): a 400 lists every refusal and leaves the
        // settings as they were. Keys the registry does not know (apPass,
        // huForget, a newer file's) are not refused. The ~5 kB of working
        // space lives only for this request, in PSRAM where there is some
        // (Scratch): not for httpd's stack, and not static (internal RAM).
        struct Work {
            Cfg::SettingsJson::Read   rd;
            Cfg::SettingsJson::Errors er;
            char err[Cfg::SettingsJson::Errors::MAX * Cfg::SettingsJson::Errors::LEN];
        };
        Scratch<Work> w;
        if (!w) { out = "no_memory"; return 503; }
        Cfg::SettingsJson::Read& rd = w->rd;
        Cfg::SettingsJson::Errors& er = w->er;
        Cfg::SettingsJson::read(m, d, rd, er, file);
        // The caller's own refusals (a file's knobs), in the same list.
        if (extra) {
            for (uint8_t i = 0; i < extra->n; ++i) er.add("%s", extra->line[i]);
            er.more = (uint16_t)(er.more + extra->more);
        }
        if (er.any()) {
            er.join(w->err, sizeof(w->err));
            out = w->err;
            return 400;
        }

        // A dry run: what would change, nothing applied (the table copies are
        // this handler's own).
        if (dry) {
            JsonDocument o(&PsramJson::instance());
            o["ok"] = true;
            bool reboot = false;
            JsonArray chg = o["changes"].to<JsonArray>();
            for (uint8_t i = 0; i < rd.n; ++i)
                if (Cfg::SettingsJson::differs(m, rd.ch[i])) {
                    chg.add(rd.ch[i].e->id);
                    if (rd.ch[i].e->apply & Cfg::AP_REBOOT) reboot = true;
                }
            for (uint8_t i = 0; i < rd.nTables; ++i) chg.add(rd.tables[i]->id);
            o["reboot"]  = reboot;
            o["unknown"] = rd.unknown;
            JsonArray sk = o["skipped"].to<JsonArray>();
            for (uint8_t i = 0; i < rd.nSkipped; ++i) sk.add(rd.skipped[i]->id);
            serializeJson(o, out);
            type = "application/json";
            return 200;
        }

        const bool huFirst = s.huTitleFirst;
        const auto src = s.mediaSource;


        uint8_t ap = 0;
        Cfg::SettingsJson::apply(m, rd, &ap);
        bool restart = (ap & Cfg::AP_REBOOT) != 0;

        // The emitter, for the hu task: the tuning as a whole block (replace,
        // not merge, so a concurrent edit is never half-applied) and the
        // message switches (RAM only). The tuning goes to NVS when it changed.
        bool huBusy = false;
        if (ap & Cfg::AP_HU) {
            huBusy = !App::Hu::emitterCfg().edit([&](Canbox::HuConfig& c) {
                memcpy(c.tuning, hu.tuning, sizeof(c.tuning));
                memcpy(c.msgOn, hu.msgOn, sizeof(c.msgOn));
            });
            if (huBusy) LOGW("cfg", "settings: the head-unit emitter was busy, its part not applied");
            else if (memcmp(hu.tuning, huWas, sizeof(huWas))) {
                _cfg->saveTuning(Canbox::activeProtocol(), hu.tuning);
                LOGI("cfg", "canbox tuning saved (survives a power cycle)");
            }
        }

        // Staged for the tasks that own them: the debounce timing and the
        // encoder direction for the matrix sweep; the pulse, the links and
        // the class masks for the keys task.
        if (ap & Cfg::AP_KEYS) {
            if (_st.keymap) {
                const Input::KeyDebouncer::Params t = s.timing;
                const bool inv = s.encInverted;
                _st.keymap->edit([&](App::KeysTask::KeyMapCfg& k) { k.timing = t; k.encInverted = inv; });
            }
            if (_st.routing) {
                const uint16_t pulse = s.pulseMs;
                const bool on = s.uartEnabled, resOn = s.resEnabled;
                const uint8_t u = s.routeUart, a = s.routeAms, h = s.routeHid, rs = s.routeRes,
                              us = s.routeUsb;
                if (!_st.routing->edit([&](App::KeysTask::Routing& k) {
                        k.pulseMs = pulse; k.uartOn = on; k.resOn = resOn;
                        k.uartCls = u; k.amsCls = a; k.hidCls = h; k.resCls = rs; k.usbCls = us;
                    }))
                    LOGW("cfg", "settings: the keys task's routing was busy, applied at the next change");
                LOGI("key", "routing: uart=0x%02X ams=0x%02X hid=0x%02X res=0x%02X usb=0x%02X",
                               u, a, h, rs, us);
            }
#if FEATURE_RESISTIVE_KEYS
            // A rung held under a mask just changed would never see its Release.
            if (_res && _st.resAllOff) _st.resAllOff();
#endif
            App::KeyBus::kick();
        }

#if FEATURE_DISPLAY_CAN
        // The screen, the pane and the key popup, live.
        if ((ap & Cfg::AP_UI) && !App::Ui::stageFrom(s))
            LOGW("cfg", "settings: the screen's were busy, applied at the next change");

        // Where the now-playing text comes from: live, so sources can be
        // compared with the panel in view. Which commands carry it is the
        // protocol's (raise.titleCmd, applied by the hu task).
        if (ap & Cfg::AP_MEDIA) {
            if (_router && s.mediaSource != src) _router->setMode(MediaRouter::modeFromStr(s.mediaSource.c_str()));
            if (_hu && s.huTitleFirst != huFirst) _hu->setTitleFirst(s.huTitleFirst);
        }
        // "Throw away what you learned and watch again", for when the learner
        // latched onto a status frame that happened to look like text.
        if (!file && (d["huForget"] | false)) {
            if (_huRx) _huRx->forgetText();
            if (_hu) _hu->forget();
        }
#endif

#if FEATURE_KNOBS
        if ((ap & Cfg::AP_KNOBS) && _knobs) {
            _knobs->setConfig(kc);
            _cfg->saveKnobs(kc);
        }
#endif

#if FEATURE_CAPTURE
        // Clamped and saved to its own NVS namespace by the recorder.
        if (ap & Cfg::AP_CAP) Capture::setSettings(cs);
#endif

        if (apPass.length()) { restart = restart || apPass != s.apPass.c_str(); s.apPass = apPass; }

        _cfg->save();
        LOGI("cfg", "settings saved: %u value(s)%s%s", (unsigned)rd.n, file ? " from a file" : "",
                       restart ? ", pending reboot" : "");
        // A file reports a part that did not take as part of a 200, so its
        // caller still applies the knobs and the page says exactly what
        // stayed behind; the form keeps its statuses.
        if (file) {
            JsonDocument o(&PsramJson::instance());
            o["ok"] = true;
            if (restart) o["pendingReboot"] = true;
            if (huBusy) o["partial"].to<JsonArray>().add("hu_busy");
            serializeJson(o, out);
            type = "application/json";
            return 200;
        }
        if (huBusy) { out = "hu_busy"; return 503; }   // its part not applied; the rest saved
        type = "application/json";
        out = restart ? "{\"ok\":true,\"pendingReboot\":true}" : "{\"ok\":true}";
        return 200;

}

void WebUi::routesSettings()
{
#if FEATURE_MATRIX
    // The 3x3 keypad's map (no page since ui-v2 slice 8).
    route("/api/keymap", HTTP_GET, [this](PsychicRequest* r) {
        // The newest edit, applied or not: what the page just saved.
        App::KeysTask::KeyMapCfg k;
        if (!_st.keymap || !_st.keymap->peek(k)) return r->reply(503, "text/plain", "busy");
        const KeyMap* const map = &k.map;
        JsonDocument d(&PsramJson::instance());
        JsonArray slots = d["slots"].to<JsonArray>();
        for (uint8_t i = 0; i < KeyMap::POSITIONS; ++i) {
            const Slot& s = map->slot(i);
            JsonObject o = slots.add<JsonObject>();
            o["pos"]       = i;
            o["action"]    = name(s.action);
            o["code"]      = s.code;
            o["effective"] = s.code ? s.code : Canbox::activeProtocol().keyCodeFor(s.action);
        }
        const uint8_t cw = map->encCode(true), ccw = map->encCode(false);
        const Canbox::Protocol& P = Canbox::activeProtocol();
        d["encCw"]       = cw  ? cw  : P.keyCodeFor(Keys::Action::EncCw);
        d["encCcw"]      = ccw ? ccw : P.keyCodeFor(Keys::Action::EncCcw);
        d["encInverted"] = k.encInverted;
        return replyDoc(r, d);
    });

    // Staged: the task that sweeps the matrix applies it (and with it the
    // encoder phases and the fast-debounce mask) at its next pass.
    route("/api/keymap", HTTP_POST, [this](PsychicRequest* r) {
        if (!_st.keymap) return r->reply(503, "text/plain", "not_built keymap");
        JsonDocument d(&PsramJson::instance());
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");

        _cfg->s().encInverted = d["encInverted"] | false;
        const bool inv = _cfg->s().encInverted;
        App::KeysTask::KeyMapCfg saved;
        const bool ok = _st.keymap->edit([&](App::KeysTask::KeyMapCfg& k) {
            for (JsonObject o : d["slots"].as<JsonArray>()) {
                uint8_t pos = o["pos"] | 0xFF;
                if (pos >= KeyMap::POSITIONS) continue;
                k.map.setSlot(pos, Slot{fromName(o["action"] | "None"),
                                        (uint8_t)(o["code"] | 0)});
            }
            k.map.setEncCode(true,  (uint8_t)(d["encCw"]  | 0));
            k.map.setEncCode(false, (uint8_t)(d["encCcw"] | 0));
            k.encInverted = inv;
            saved = k;
        });
        if (!ok) return r->reply(503, "text/plain", "busy");
        App::KeyBus::kick();
        _cfg->saveKeyMap(saved.map);
        _cfg->save();
        LOGI("cfg", "keymap saved");
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    route("/api/keymap/defaults", HTTP_POST, [this](PsychicRequest* r) {
        if (!_st.keymap) return r->reply(503, "text/plain", "not_built keymap");
        App::KeysTask::KeyMapCfg saved;
        if (!_st.keymap->edit([&](App::KeysTask::KeyMapCfg& k) { k.map.loadDefaults(); saved = k; }))
            return r->reply(503, "text/plain", "busy");
        App::KeyBus::kick();
        _cfg->saveKeyMap(saved.map);
        LOGI("cfg", "keymap reset to Megane defaults");
        return r->reply(200, "application/json", "{\"ok\":true}");
    });
#endif

    // ---- every setting the registry describes (src/cfg/SettingsRegistry.h) ----
    // The same table the panel menu draws: a key here is a registry id, a
    // Bool is true/false, an Int a number, an Enum its stored id. A Table is
    // a list of rows, each a list of cells in column order (the catalog's
    // `cols`), cells as above; a scaled Int has its decimal point placed.
    route("/api/settings", HTTP_GET, [this](PsychicRequest* r) {
        auto& s = _cfg->s();
        JsonDocument d(&PsramJson::instance());
        settingsOut(d.to<JsonObject>(), false);
        // Not settings: the network names (passwords never) and the build.
        d["ssid"]           = s.ssid.c_str();
        d["ssid2"]          = _wifi ? _wifi->net2Ssid() : String();
        // fw is the build; cfgVer is the NVS schema version, not a build version.
        d["fw"]             = MEGAOPEN_VERSION;
        d["build"]          = MegaOpen::buildId();
        d["cfgVer"]         = _cfg->version();
        d["cfgVerWas"]      = _cfg->migratedFrom();
        d["wifiSleep"]      = _wifi ? _wifi->sleeping() : false;   // derived, read-only
        return replyDoc(r, d);
    });

    route("/api/settings", HTTP_POST, [this](PsychicRequest* r) {
        JsonDocument d(&PsramJson::instance());
        if (deserializeJson(d, r->body()) || !d.is<JsonObjectConst>())
            return r->reply(400, "text/plain", "bad_json");
        String out;
        const char* type = "text/plain";
        const int st = settingsPost(d.as<JsonObjectConst>(), paramStr(r, "dry") == "1", false, out, type);
        return r->reply(st, type, out.c_str());
    });

    // ---- the settings file ----------------------------------------------------
    // GET: every setting a file should carry (no read-only, no RAM-only ones)
    // and the knob configuration, with the schema and the build that wrote it.
    // POST: such a file, through the same checks as POST /api/settings, all or
    // nothing; ?dry=1 answers what would change and changes nothing.
    route("/api/config", HTTP_GET, [this](PsychicRequest* r) {
        // Streamed (ChunkOut): the settings and the knobs each as their own
        // short-lived document, never the whole file as one String.
        ChunkOut out(r);
        ChunkWriter w{out};
        char head[160];
        snprintf(head, sizeof(head), "{\"format\":\"%s\",\"schema\":%d,\"fw\":\"%s\",\"build\":\"%s\",\"settings\":",
                 Cfg::SettingsJson::FORMAT, Cfg::SettingsJson::SCHEMA, MEGAOPEN_VERSION, MegaOpen::buildId());
        out.put(head);
        {
            JsonDocument d(&PsramJson::instance());
            settingsOut(d.to<JsonObject>(), true);
            serializeJson(d, w);
        }
#if FEATURE_KNOBS
        if (_knobs) {
            out.put(",\"knobs\":");
            JsonDocument d(&PsramJson::instance());
            knobsJson(_knobs->config(), d.to<JsonObject>());
            serializeJson(d, w);
        }
#endif
        mapToFile(out);   // the CAN map and the head-unit emitter
#if FEATURE_DISPLAY_CAN
        // The Custom scene's picture, when one was drawn: it lives only in
        // this board's NVS, so a reset or another board would lose it.
        if (_ui.paneBits) {
            struct Pic { uint8_t bits[MegaOpen::MediaPane::BYTES]; char hex[MegaOpen::MediaPane::BYTES * 2 + 1]; bool set; };
            Scratch<Pic> pic;   // ~870 B: not on httpd's stack
            if (pic) {
                pic->set = false;
                _ui.paneBits->read([&](const App::Ui::PaneBits& p) { pic->set = p.set; memcpy(pic->bits, p.bits, sizeof(pic->bits)); });
                if (pic->set) {
                    MegaOpen::MediaPane::toHex(pic->bits, pic->hex);
                    out.put(",\"picture\":\"");
                    out.put(pic->hex);
                    out.put("\"");
                }
            }
        }
#endif
        out.put("}");
        return out.finish();
    });

    route("/api/config", HTTP_POST, [this](PsychicRequest* r) {
        JsonDocument d(&PsramJson::instance());
        if (deserializeJson(d, r->body()) || !d.is<JsonObjectConst>())
            return r->reply(400, "text/plain", "bad_json");
        const auto chk = Cfg::SettingsJson::checkFile(d.as<JsonObjectConst>());
        if (chk != Cfg::SettingsJson::FileCheck::Ok)
            return r->reply(400, "text/plain", Cfg::SettingsJson::fileCheckText(chk));
        const bool dry = paramStr(r, "dry") == "1";

        // The knobs first, onto a copy: a name that is no row is a refusal,
        // listed with the settings' own (settingsPost's `extra`), and what
        // sanitize() had to bring into range is shown in the preview. The
        // copy is applied only after the settings were.
        JsonVariantConst kj = d["knobs"];
        Scratch<Cfg::SettingsJson::Errors> kerrBox;
        if (!kerrBox) return r->reply(503, "text/plain", "no_memory");
        Cfg::SettingsJson::Errors& kerr = *kerrBox.get();
        bool knobs = false;
        uint16_t clamped = 0;
#if FEATURE_KNOBS
        Knobs::Config kc;
        knobs = _knobs && kj.is<JsonObjectConst>();
        if (knobs) {
            kc = _knobs->config();
            clamped = knobsFromJson(kj, kc, &kerr);
        }
#endif
        // The Custom scene's picture (576 hex), checked now, saved after the
        // settings. A build without the panel skips it.
        JsonVariantConst pj = d["picture"];
#if FEATURE_DISPLAY_CAN
        const bool picIn = !pj.isNull() && _ui.paneBits;
        if (!pj.isNull() && (!pj.is<const char*>() || !MegaOpen::MediaPane::fromHex(pj.as<const char*>(), nullptr)))
            return r->reply(400, "text/plain", "param picture");
#else
        const bool picIn = false;
#endif
        // The car's tables (the CAN map, the head-unit fields, the formulas,
        // the rules) are checked before anything is applied, like the rest;
        // applied after the settings.
        FileParts car;
        if (const char* why = mapFromFile(d.as<JsonObjectConst>(), true, car))
            return r->reply(400, "text/plain", why);
        String out;
        const char* type = "text/plain";
        const int st = settingsPost(d["settings"].as<JsonObjectConst>(), dry, true, out, type, &kerr);
        if (st != 200) return r->reply(st, type, out.c_str());
        if (!dry && car.any())
            if (const char* why = mapFromFile(d.as<JsonObjectConst>(), false, car))
                return r->reply(500, "text/plain", why);
#if FEATURE_DISPLAY_CAN
        if (!dry && picIn) {
            const char* hex = pj.as<const char*>();
            if (!_ui.paneBits->edit([&](App::Ui::PaneBits& p) { MegaOpen::MediaPane::fromHex(hex, p.bits); p.set = true; }))
                return r->reply(503, "text/plain", "busy");
            bool saved = false;
            _ui.paneBits->read([&](const App::Ui::PaneBits& p) { saved = _cfg->savePaneBits(p.bits, sizeof(p.bits)); });
            if (!saved) return r->reply(500, "text/plain", "nvs_failed");
            LOGI("media", "own picture set from a settings file");
        }
#endif

        JsonDocument o(&PsramJson::instance());
        deserializeJson(o, out);
        if (dry) {
            o["knobs"] = knobs;
            o["map"]      = car.map;        // the file carries the CAN map
            o["hu"]       = car.hu;         // what the board sends to the head unit
            o["formulas"] = car.formulas;   // the formulas
            o["rules"]    = car.rules;      // the rules
            o["picture"] = picIn;    // and the owner's own picture
            JsonArray cl = o["clamped"].to<JsonArray>();
#if FEATURE_KNOBS
            for (uint16_t b = 1; b; b = (uint16_t)(b << 1))
                if (clamped & b) cl.add(Knobs::Config::clampName(b));
#else
            (void)cl; (void)clamped;
#endif
            o["fileFw"] = d["fw"] | "";
        }
#if FEATURE_KNOBS
        else if (knobs) {
            _knobs->setConfig(kc);
            _cfg->saveKnobs(kc);
            LOGI("knobs", "set from a settings file");
        }
#endif
        return replyDoc(r, o);
    });
}

void WebUi::routesWifi()
{
    // Blocks for a couple of seconds; explicit request only.
    route("/api/wifi/scan", HTTP_POST, [this](PsychicRequest* r) {
        if (_wifi) _wifi->scanBegin();
        const int n = WiFi.scanNetworks();
        JsonDocument d(&PsramJson::instance());
        JsonArray a = d["networks"].to<JsonArray>();
        for (int i = 0; i < n && i < 30; ++i) {
            JsonObject o = a.add<JsonObject>();
            o["ssid"]   = WiFi.SSID(i);
            o["rssi"]   = WiFi.RSSI(i);
            o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        }
        WiFi.scanDelete();
        if (_wifi) _wifi->scanEnd();
        d["count"] = n;
        d["current"] = WiFi.isConnected() ? WiFi.SSID() : String();
        return replyDoc(r, d);
    });

    // {ssid, pass, ssid2, pass2}, either pair or both. An empty password keeps
    // the stored one while the name stays the same; a new name with no password
    // is an open network. The first network applies at boot, the second live.
    route("/api/wifi", HTTP_POST, [this](PsychicRequest* r) {
        JsonDocument d(&PsramJson::instance());
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        const bool has1 = d["ssid"].is<const char*>(), has2 = d["ssid2"].is<const char*>();
        const String s1 = d["ssid"] | "", s2 = d["ssid2"] | "";
        String p1 = d["pass"] | "";
        const String p2 = d["pass2"] | "";
        if (!has1 && !has2) return r->reply(400, "text/plain", "param ssid");
        if (s1.length() > 32 || s2.length() > 32)
            return r->reply(400, "text/plain", "param ssid");
        auto bad = [](const String& p) { return p.length() && (p.length() < 8 || p.length() > 64); };
        if (bad(p1) || bad(p2)) return r->reply(400, "text/plain", "pass_len");
        if (has2 && !_wifi) return r->reply(503, "text/plain", "not_built wifi_manager");
        auto& s = _cfg->s();
        // Only the first network needs the reboot: WifiManager takes the second
        // (setNet2) at its next AP retry.
        const bool changed = has1 && (s1 != s.ssid.c_str() || (p1.length() && p1 != s.pass.c_str()));
        if (has1) {
            if (p1.isEmpty() && s1 == s.ssid.c_str()) p1 = s.pass.c_str();
            s.ssid = s1;
            s.pass = p1;
            _cfg->save();
        }
        if (has2) _wifi->setNet2(s2, p2);   // the same rule for an empty password
        LOGI("wifi", "%s", changed ? "saved, pending reboot" : "saved");
        return r->reply(200, "application/json", changed ? "{\"ok\":true,\"pendingReboot\":true}"
                                                         : "{\"ok\":true}");
    });
}
