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


// GET /api/status's object; also the status snapshot LogPush sends to GitHub.
void WebUi::statusJson(JsonObject d)
{
        d["ip"]        = WiFi.isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
        d["mode"]      = _wifi && _wifi->off() ? "off" : WiFi.isConnected() ? "sta" : "ap";
        d["ssid"]      = WiFi.isConnected() ? WiFi.SSID() : WiFi.softAPSSID();
        d["rssi"]      = WiFi.RSSI();
        d["uptimeMs"]  = millis();
        d["fw"]        = MEGAOPEN_VERSION;
        d["build"]     = MegaOpen::buildId();
        d["boots"]     = MegaOpen::bootNo();
        d["apName"]    = _wifi ? _wifi->apSsid() : String();
        d["framesSent"]= _uart->framesSent();
        d["events"]    = _engine->events();
        d["ghosts"]    = _ghosts ? *_ghosts : 0;
        d["encSteps"]  = _encSteps ? *_encSteps : 0;
        d["heap"]      = ESP.getFreeHeap();
        {
            // Failed allocations since boot (src/utils/AllocWatch.h), the last one's detail.
            AllocWatch::Snap f;
            AllocWatch::get(f);
            JsonObject o = d["allocFail"].to<JsonObject>();
            o["n"] = f.count;
            if (f.count) {
                o["size"] = f.size;
                o["caps"] = f.caps;
                o["atMs"] = f.atMs;
                o["task"] = f.task;
            }
        }
#if FEATURE_LOG_PUSH
        {
            // The push's counters; held = waits NetBudget asked for (src/net/NetBudget.h).
            LogPush::View v;
            LogPush::view(v);
            JsonObject o = d["push"].to<JsonObject>();
            o["state"] = LogPush::stateId(v.state);
            o["files"] = v.files;
            o["held"]  = v.held;
        }
#endif
#if FEATURE_APP_LINK
        {
            AppLink::Stats a;
            AppLink::stats(a);
            d["linkVia"] = AppLink::viaName(a.via);
            // One object per transport; the counters are the session's
            // (Link::SessionStats), then what only that transport knows.
            auto session = [&](JsonObject o, const Link::SessionStats& u) {
                o["up"]      = u.up;
                o["alive"]   = u.alive;
                o["hello"]   = u.hello;
                o["open"]    = u.open;
                o["reqs"]    = u.reqs;
                o["fails"]   = u.fails;
                o["events"]  = u.events;
                o["rx"]      = u.rxFrames;
                o["tx"]      = u.txFrames;
                o["bad"]     = u.bad;
                o["txWaitMax"]   = u.txWaitMax;
                o["connectMax"]  = u.connectMax;
                o["sockSendMax"] = u.sockSendMax;
                o["cut"]         = u.cut;
                o["dropped"]     = u.dropped;
                if (u.lastRxAgoMs == UINT32_MAX) o["lastRxAgoMs"] = nullptr;
                else                             o["lastRxAgoMs"] = u.lastRxAgoMs;
            };
#if FEATURE_USB_LINK
            {
                JsonObject o = d["usb"].to<JsonObject>();
                session(o, a.usb);
                o["mounted"]     = a.bus.mounted;
                o["mounts"]      = a.bus.mounts;
                o["unmounts"]    = a.bus.unmounts;
                o["suspends"]    = a.bus.suspends;
                o["resumes"]     = a.bus.resumes;
                // The bus itself, whatever TinyUSB reported (UsbPipe.h
                // BusStats, docs/design/usb-link.md "Diagnostics").
                o["sof"]         = a.bus.sof;
                if (a.bus.sofAgoMs == UINT32_MAX) o["sofAgoMs"] = nullptr;
                else                              o["sofAgoMs"] = a.bus.sofAgoMs;
                o["busUps"]      = a.bus.busUps;
                o["addressed"]   = a.bus.addressed;
                o["suspended"]   = a.bus.suspended;
            }
#endif
#if FEATURE_BLE_LINK
            {
                JsonObject o = d["ble"].to<JsonObject>();
                session(o, a.ble);
                o["on"]            = a.peer.on;
                o["subscribed"]    = a.peer.subscribed;
                o["mtu"]           = a.peer.mtu;
                o["peer"]          = a.peer.addr;
                o["subscribes"]    = a.peer.subscribes;
                o["overflow"]      = a.peer.overflow;
                o["unencrypted"]   = a.peer.unencrypted;
                o["notifyRetries"] = a.peer.notifyRetries;
            }
#endif
        }
#endif
}

// GET /api/health's object; also the status snapshot LogPush sends to GitHub.
void WebUi::healthJson(JsonObject d)
{
        d["fw"] = MEGAOPEN_VERSION;
        // Which flash: the version above is the same in every image.
        d["build"] = MegaOpen::buildId();
        JsonArray chains = d["chains"].to<JsonArray>();

        // "ok" it works right now · "idle" wired up, nothing to do · "off"
        // deliberately switched off · "broken" should work and does not.
        // `code` is stable and the only explanation sent: the page has the
        // words for it in both languages (web/i18n/*.json, "w.<code>").
        auto chain = [&](const char* id, const char* state, const char* code) {
            JsonObject o = chains.add<JsonObject>();
            o["id"] = id;
            o["state"] = state;
            o["code"] = code;
            return o;
        };

        // _phoneConnected (AMS/CTS link) exists only in display builds.
#if FEATURE_DISPLAY_CAN
        const bool phone = _phoneConnected ? _phoneConnected() : false;
#else
        const bool phone = false;
        (void)phone;
#endif

#if FEATURE_DISPLAY_CAN
        // The panel task's copy: httpd never asks the library itself. The ui
        // task's counters, settings and screen likewise: copies only.
        const MegaOpen::PanelStatus ps = _panel ? _panel->read() : MegaOpen::PanelStatus{};
        const App::Ui::UiStatus us = _ui.status ? _ui.status->read() : App::Ui::UiStatus{};
        App::Ui::MediaSettings ms;
        const bool haveMs = mediaSettings(ms);
        // ~320 bytes, not on the stack and not static: for this request only
        // (Scratch). No memory reads as no media; every use checks haveMedia.
        Scratch<MegaOpen::MediaScreen> box;
        const bool haveMedia = box && _ui.media && _ui.media->copy(*box);
        // 1. The panel. Registered is the health signal; PeerAlive is omitted
        // because it toggles every second (sync 0x0A and 0x08 are both healthy).
        {
            static constexpr const char* W[] = {
                "panel_off",
                "panel_unregistered",
                "panel_ok",
                "panel_foreign_master",
                "panel_cluster_only",
                "panel_beside_radio",
                "panel_no_radio"};
            const bool reg = ps.attached && ps.registered;
            // What the bus says against the role (display/PanelRole.h).
            // Extension: the stock radio's sync is the expected bus, and its
            // absence means nothing of ours shows. Emulator: another master's
            // sync means two radios fight over one panel; a lone cluster
            // means no AFFA panel to register with.
            const bool extension = ps.role == (uint8_t)MegaOpen::PanelRole::Extension;
            const bool foreign = _panelWatch && _panelWatch->foreignMaster(millis());
            const bool cluster = !reg && _panelWatch && _panelWatch->clusterOnly();
            const uint8_t i = !ps.attached ? 0
                            : extension    ? (foreign ? 5 : 6)
                            : foreign      ? 3 : reg ? 2 : cluster ? 4 : 1;
            JsonObject o = chain("panel", i == 0 ? "off" : (i == 2 || i == 5) ? "ok" : "broken", W[i]);
            o["role"] = MegaOpen::PanelRoles::id((MegaOpen::PanelRole)ps.role);
            if (_panelWatch) {
                o["foreignMasters"] = _panelWatch->masters();
                o["clusterPings"]   = _panelWatch->clusterPings();
            }
            o["rxFrames"] = _canRx ? *_canRx : 0;
            o["rxPin"]    = (int)Board::PIN_CAN_RX;
            o["txPin"]    = (int)Board::PIN_CAN_TX;
            // How often the link has dropped, and how long ago.
            if (_ui.status) o["resyncs"] = us.resyncs;
            if (us.lastResyncMs)
                o["sinceResyncMs"] = (uint32_t)(millis() - us.lastResyncMs);
            // Shut while nothing on the bus ACKs us (the panel task's TX gate).
            o["txGated"] = ps.txGated;
        }

        // 2. Phone metadata onto the glass; `why` names the first missing precondition.
        {
            const bool on    = haveMs && ms.mediaAuto;
            const bool reg   = ps.attached && ps.registered;
            const bool haveT = haveMedia && box->title()[0];
            static constexpr const char* W[] = {
                "media_off",
                "media_no_panel",
                "media_no_phone",
                "media_no_track",
                "media_ok"};
            const uint8_t i = !on ? 0 : !reg ? 1 : !phone ? 2 : !haveT ? 3 : 4;
            JsonObject o = chain("media", !on ? "off" : i < 4 ? "idle" : "ok", W[i]);
            if (haveMedia) {
                o["title"]  = box->title();
                o["artist"] = box->artist();
                o["clock"]  = box->clock();
            }
            o["delivered"]  = ps.delivered;
            o["superseded"] = ps.superseded;
            o["failed"]     = ps.failed;
        }

        // 3. The clock. Separate from media: the panel draws its clock even with
        //    the display off.
        {
            //    Sources: the phone and the head unit's 0x9D (ClockHealth.h).
            const App::Ui::ClockHealth h =
                App::Ui::clockHealth(haveMs && ms.autoTime, phone, us.clockSrc, millis() - us.clockSetMs);
            JsonObject o = chain("clock", h.state, h.code);
            o["src"] = App::Ui::clockSrcName(us.clockSrc);
            if (haveMedia) o["hhmm"] = box->clock();
        }
#endif

        // 4. Keys, one entry per destination.
        {
            const bool any = _keyEvents && *_keyEvents;
            JsonObject o = chain("keys", any ? "ok" : "idle",
                                 any ? "keys_ok" : "keys_none");
            if (_keyEvents) o["events"] = *_keyEvents;
            App::KeysTask::Routing k;
            if (_st.routing && _st.routing->peek(k)) putDests(o["dest"].to<JsonObject>(), k);
        }

        // 5. Notifications.
        {
            const uint32_t n = _ancsCount ? *_ancsCount : 0;
#if FEATURE_BLE && FEATURE_ANCS
            static constexpr const char* W[] = {
                "ancs_ok",
                "ancs_no_phone",
                "ancs_waiting"};
            JsonObject o = chain("ancs", n ? "ok" : "idle", W[n ? 0 : !phone ? 1 : 2]);
            o["count"] = n;
            o["popup"] = _cfg->s().ancsPopup;
#else
            chain("ancs", "off", "ancs_off");
#endif
        }

        // 6. The head unit on the UART: alive while a frame came within
        //    HuMediaSource's link timeout. Builds without the panel have no
        //    HuMediaSource: hu_seen, a frame since boot, recency unknown.
        {
            static constexpr const char* W[] = {
                "hu_off",
                "hu_silent",
                "hu_quiet",
                "hu_ok",
                "hu_seen",
                "hu_dialect_vw"};
            const Canbox::DecoderStats rs = _huRx ? _huRx->stats() : Canbox::DecoderStats{};
            const uint32_t n = rs.frames;
#if FEATURE_DISPLAY_CAN
            const uint8_t live = !_hu ? 4 : _hu->heard(millis()) ? 3 : 2;
#else
            const uint8_t live = 4;
#endif
            // Wired and talking, but in a profile the protocol does not speak:
            // nothing it sends is understood and nothing we send is shown
            // (Raise: set the unit's canbox profile to Toyota RAV4).
            const bool vw = _huRx && _huRx->wrongDialect();
            const uint8_t i = !_cfg->s().uartEnabled ? 0 : !n ? 1 : vw ? 5 : live;
            JsonObject o = chain("hu", i == 0 ? "off" : i == 3 ? "ok" : i == 5 ? "broken" : "idle", W[i]);
            o["rxFrames"] = n;
            if (_huRx) {
                o["dialect"] = _huRx->dialect();
                // Frames of ours the unit says it does not support (Raise 0xF3).
                o["notSupported"] = rs.notSupported;
            }
            // The protocol's own counters (Raise: climateChanges, 0x28 frames
            // that changed, each may pop the climate overlay). Read as is.
            if (_emitter)
                for (uint8_t k = 0; k < _emitter->statCount(); ++k) {
                    const Canbox::EmitterStat st = _emitter->stat(k);
                    o[st.id] = st.value;
                }
            // Wiring check: nothing at all = wrong pin or no ground; junk and
            // no frames = wired, wrong speed or dialect; bad checksums = noise.
            o["junk"]   = rs.junkBytes;
            o["badCs"]  = rs.badChecksums;
        }

#if FEATURE_KNOBS
        // 6b. The knobs and their rings, for plugging them in one at a time.
        if (_knobs) {
            const Knobs::Service::Live v = _knobs->live();
            const Knobs::Config c = _knobs->config();
            const bool found = !Board::KNOBS_I2C || v.expAddr;
            JsonObject o = chain("knobs", found ? "ok" : "broken",
                                 found ? "knobs_ok"
                                       : "knobs_no_expander");
            o["addr"]      = v.expAddr;
            o["i2cErrors"] = v.expErrors;
            o["right"]     = v.pos[0];
            o["left"]      = v.pos[1];
            o["clicks"]    = v.clicks + v.clicks2;
            o["lineErrors"] = v.encErrors + v.enc2Errors;

            JsonObject l = chain("lights", !c.ledEnabled ? "off" : v.stripOk ? "ok" : "broken",
                                 !c.ledEnabled ? "lights_off"
                                 : v.stripOk   ? "lights_ok"
                                               : "lights_fail");
            l["leds"]   = v.leds;
            l["frames"] = v.frames;
        }
#endif

#if FEATURE_APP_LINK
        // 6c. MegaOpen Companion: on which link it is, and what it posted.
        {
            const AppLink::Via via = AppLink::via();
            const bool bleOff = _cfg->s().bleMode == "off";
            JsonObject o = chain("app", via != AppLink::Via::None ? "ok" : "idle",
                                 via == AppLink::Via::Usb ? "app_usb"
                                 : via == AppLink::Via::Ble ? "app_ble"
                                 : bleOff ? "app_none_ble_off" : "app_none");
            o["via"] = AppLink::viaName(via);
            o["net"] = AppLink::netAvailable();
            if (_appMedia) {
                o["mediaPosts"] = _appMedia->posts();
                const uint32_t last = _appMedia->lastPostMs();
                if (_appMedia->posts()) o["mediaAgoMs"] = millis() - last;
                else                    o["mediaAgoMs"] = nullptr;
            }
        }
#endif

        // 7. The phone link itself, last, because everything above cites it.
        chain("phone", phone ? "ok" : "idle",
              phone ? "phone_ok"
              : _cfg->s().bleMode == "off" ? "phone_ble_off"
                                           : "phone_advertising");
        d["bleMode"] = _cfg->s().bleMode.c_str();
}
