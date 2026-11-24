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
#include "../ble/apple_notification_service.h"
#endif

using namespace Keys;
#include "WebUiKit.h"

#if FEATURE_DISPLAY_CAN
// ---- the now-playing screen ---------------------------------------------------
// The newest settings, applied or not: what the page should show.
bool WebUi::mediaSettings(App::Ui::MediaSettings& out) const
{
    return _ui.settings && _ui.settings->peek(out);
}

void WebUi::routesMedia()
{
    route("/api/media", HTTP_GET, [this](PsychicRequest* r) {
        if (!_ui.media || !_ui.settings) return r->reply(503, "text/plain", "not_built display");
        // Copies only: the ui task owns the screen and the pane. Not on the
        // stack (~320 + ~1200 bytes on a 4 kB httpd stack) and not static
        // (internal RAM): for this request only (Scratch).
        Scratch<MegaOpen::MediaScreen> box;
        if (!box) return r->reply(503, "text/plain", "no_memory");
        MegaOpen::MediaScreen& media = *box;
        if (!_ui.media->copy(media)) return r->reply(503, "text/plain", "busy");
        App::Ui::MediaSettings ms;
        if (!mediaSettings(ms)) return r->reply(503, "text/plain", "busy");
        const App::Ui::UiStatus us = _ui.status ? _ui.status->read() : App::Ui::UiStatus{};

        JsonDocument d(&PsramJson::instance());
        d["auto"]     = ms.mediaAuto;
        d["title"]    = media.title();
        d["artist"]   = media.artist();
        // Empty until the phone has reported a time.
        d["clock"]    = media.clock();
        // Who set the panel's clock last (hu, cts, manual), and to what.
        d["clockSrc"] = App::Ui::clockSrcName(us.clockSrc);
        char at[6] = "";
        if (us.clockSrc != App::Ui::ClockSrc::None)
            snprintf(at, sizeof(at), "%02u:%02u", (unsigned)(us.clockAt >> 8), (unsigned)(us.clockAt & 0xFF));
        d["clockAt"]  = at;
        d["state"]    = (uint8_t)media.state();
        d["elapsed"]  = media.elapsed();
        d["duration"] = media.duration();
        d["cols"]     = ms.cols;
        d["bar"]      = ms.bar;
        // `scrollMs` is the title row, kept for existing readers.
        d["scrollMs"]       = ms.scrollMsTitle;
        d["scrollMsTitle"]  = ms.scrollMsTitle;
        d["scrollMsArtist"] = ms.scrollMsArtist;
        d["layout"]   = MegaOpen::MediaScreen::layoutName(ms.layout);
        d["splashMs"] = ms.splashMs;

        // The preview below is composed on the copy with the newest settings,
        // by the same code that drives the panel, so the page sees its own edit.
        media.setLayout(ms.layout);
        media.setCols(ms.cols);
        media.setBarWidth(ms.bar);
        media.setScrollMsTitle(ms.scrollMsTitle);
        media.setScrollMsArtist(ms.scrollMsArtist);
        media.setStyle(ms.style);
        d["style"] = MegaOpen::MediaScreen::styleName(ms.style);
        d["scroll"] = ms.scroll;
        // False when the head unit is the source: it reports no state or position.
        d["hasTransport"] = media.hasTransport();

        // ---- which source, and what the head unit has told us ---------------
        if (_router) {
            d["source"]       = MediaRouter::modeStr(_router->mode());
            d["sourceActive"] = _router->sourceName();   // what `auto` actually picked
            d["sourceLive"]   = _router->active();
            d["sourceStatus"] = _router->statusText();
            // What the source sent, UTF-8, before the panel's ASCII fold:
            // `title`/`artist` above are the glass's copy (Cyrillic transliterated).
            const MediaInfo sm = _router->snapshot();
            d["srcTitle"]  = sm.title;
            d["srcArtist"] = sm.artist;
            d["srcAlbum"]  = sm.album;
        }
        if (_hu) {
            // huFrames climbing with huTextFrames at 0: the unit talks but sends
            // no text. Both 0: dead UART. huLearned* is what the learner chose,
            // hu*Cmd is what was pinned.
            const Canbox::TextChannels tc = _huRx ? _huRx->textChannels() : Canbox::TextChannels{};
            d["huFrames"]        = _hu->frames();
            d["huTextFrames"]    = _huRx ? _huRx->stats().textFrames : 0;
            d["huLearnedTitle"]  = tc.learnedTitle;
            d["huLearnedArtist"] = tc.learnedArtist;
            d["huTitleCmd"]      = tc.titleCmd;
            d["huArtistCmd"]     = tc.artistCmd;
            d["huTitleFirst"]    = _hu->titleFirst();
            // A copy under the source's mutex: the hu task writes these strings.
            const MediaInfo hm = _hu->snapshot();
            d["huTitle"]         = hm.title;
            d["huArtist"]        = hm.artist;
            d["huAlbum"]         = hm.album;
        }

        // ---- was it drawn, or only queued -----------------------------------
        // `busy`: wire mid-transfer, so a repaint was skipped. `superseded`
        // tracking `delivered` means repaints outrun the panel.
        if (_panel) {
            const MegaOpen::PanelStatus ps = _panel->read();
            d["txBusy"]       = ps.busy;
            d["txDelivered"]  = ps.delivered;
            d["txSuperseded"] = ps.superseded;
            d["txFailed"]     = ps.failed;
            d["txLast"]       = affa::resultName((affa::Result)ps.lastDelivery);
        }

        // Where the metadata comes from ("no title" and "no phone" look the same).
        d["phone"]    = _phoneConnected ? _phoneConnected() : false;
        d["autoTime"] = ms.autoTime;
        d["bleMode"]  = _cfg ? _cfg->s().bleMode.c_str() : "off";
        if (_router) {
            // snapshot(), not current(): AMS writes MediaInfo from NimBLE's host
            // task; the copy is taken under the source's mutex.
            const MediaInfo m = _router->snapshot();
            d["srcTitle"]  = m.title;
            d["srcArtist"] = m.artist;
            d["srcAlbum"]  = m.album;
            d["srcPlayer"] = m.playerName;
            d["srcState"]  = (uint8_t)m.playbackState;
            d["srcDur"]    = (uint16_t)m.duration;
            d["srcElapsed"] = (uint16_t)m.elapsedAt(millis());
        }

        // The rows exactly as they would be sent now.
        char r1[MegaOpen::MediaScreen::ROW_MAX];
        char r2[MegaOpen::MediaScreen::ROW_MAX];
        char r3[MegaOpen::MediaScreen::ROW_MAX];
        media.compose(millis(), r1, r2, r3);
        JsonArray p = d["preview"].to<JsonArray>();
        p.add(r1); p.add(r2); p.add(r3);

        // ---- the graphic layer ------------------------------------------
        // The pane preview, composed on the board by the panel's code. 288 bytes
        // as 576 hex characters (readable, unlike base64).
        // Its ~1.2 kB of working space for this request only (Scratch), in
        // this scope because pn["bits"] points at pw->hex until serializeJson.
        // No memory: the reply goes out without the pane.
        struct PaneWork {
            MegaOpen::MediaPane snap;
            uint8_t bits[MegaOpen::MediaPane::BYTES];
            char    hex[MegaOpen::MediaPane::BYTES * 2 + 1];
        };
        Scratch<PaneWork> pw;
        if (pw && _ui.pane && _ui.pane->copy(pw->snap)) {
            JsonObject pn = d["pane"].to<JsonObject>();
            pn["scene"]     = MegaOpen::MediaPane::sceneName(pw->snap.scene());
            pn["on"]        = pw->snap.on();
            pn["animates"]  = MegaOpen::MediaPane::animates(pw->snap.scene());
            pn["usesMedia"] = MegaOpen::MediaPane::usesMedia(pw->snap.scene());
            pn["sent"]      = us.panesSent;
            pn["failed"]    = us.panesFailed;
            // Animation steps per delivered image (the image rate is the wire's).
            pn["speed"]     = ms.paneSpeed;
            // Carousel: `scene` is the live stop, `auto` whether it advances.
            pn["auto"]      = ms.paneAuto;
            pn["cycleS"]    = ms.paneCycleS;

            // Only setText and showInfoMenu leave the pane's half of the glass
            // alone; `menu` and `fullscreen` cover it while it is still delivered.
            pn["visible"] = MegaOpen::MediaScreen::sharesWithPane(ms.layout);
            if (pw->snap.on() && !MegaOpen::MediaScreen::sharesWithPane(ms.layout))
                pn["hiddenBy"] = MegaOpen::MediaScreen::layoutName(ms.layout);

            // Composing doesn't advance the animation; the copy is ours anyway.
            // Except Custom: the copy's picture pointer is the ui task's live
            // buffer (MediaPane::setCustom), which httpd must not read. The
            // picture comes from the staged copy, under its mutex, instead.
            // This is the only place httpd composes a pane.
            if (pw->snap.scene() == MegaOpen::MediaPane::Scene::Custom) {
                memset(pw->bits, 0, sizeof(pw->bits));
                if (_ui.paneBits)
                    _ui.paneBits->read([&](const App::Ui::PaneBits& p) { memcpy(pw->bits, p.bits, sizeof(pw->bits)); });
            } else if (pw->snap.scene() == MegaOpen::MediaPane::Scene::Cover) {
                // The same for the cover: the source's copy, under its mutex;
                // none, and the scene draws Media (on our copy, pointer dropped).
                if (!_appMedia || !_appMedia->art(pw->bits)) {
                    pw->snap.setCover(nullptr);
                    pw->snap.compose(pw->bits);
                }
            } else {
                pw->snap.compose(pw->bits);
            }
            MegaOpen::MediaPane::toHex(pw->bits, pw->hex);
            pn["bits"] = (const char*)pw->hex;
        }

        return replyDoc(r, d);
    });

#if FEATURE_BLE
    // BLE state. `bonded` false while advertising: pairing never completed on the
    // phone. `bonded` true, `connected` false: stale bond, forget on both sides.
    route("/api/ble", HTTP_GET, [this](PsychicRequest* r) {
        if (!_bleStatus) return r->reply(503, "text/plain", "not_built ble");
        const String s = _bleStatus();
        return replyJson(r, s);
    });

    // Forgets every bond (no per-peer delete). Clears a half-pair only if the
    // phone also does "Forget This Device".
    route("/api/ble/unbond", HTTP_POST, [this](PsychicRequest* r) {
        if (!_bleUnbond) return r->reply(503, "text/plain", "not_built ble");
        _bleUnbond();
        LOGI("ble", "all bonds cleared — now Forget This Device on the phone too");
        return r->reply(200, "application/json",
                        "{\"ok\":true,\"note\":\"forget it on the phone as well\"}");
    });
#endif

    // ---- now playing from the head unit's app (docs/design/companion-media.md)
    // MegaOpen Companion posts what plays on the head unit, on every change
    // and every 20 s; the same over USB, BLE and WiFi (it is plain HTTP).
    route("/api/app/media", HTTP_POST, [this](PsychicRequest* r) {
        if (!_appMedia) return r->reply(503, "text/plain", "not_built media");
        JsonDocument d(&PsramJson::instance());
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        const char* st = d["state"] | "";
        AppMediaSource::Update u;
        if      (!strcmp(st, "playing")) u.state = AppMediaSource::State::Playing;
        else if (!strcmp(st, "paused"))  u.state = AppMediaSource::State::Paused;
        else if (!strcmp(st, "none"))    u.state = AppMediaSource::State::None;
        else return r->reply(400, "text/plain", "param state");
        u.app    = d["app"] | "";
        u.title  = d["title"] | "";
        u.artist = d["artist"] | "";
        u.album  = d["album"] | "";
        const int64_t pos = d["pos"] | (int64_t)0, dur = d["dur"] | (int64_t)0;
        u.posMs  = pos < 0 ? 0 : pos > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)pos;
        u.durMs  = dur < 0 ? 0 : dur > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)dur;
        u.rate   = d["rate"] | 1.0f;
        const char* art = d["art"] | "";
        if (art[0]) {
            char* end = nullptr;
            const unsigned long h = strtoul(art, &end, 16);
            if (strlen(art) != 8 || !end || *end) return r->reply(400, "text/plain", "param art");
            u.artHash = (uint32_t)h;
        }
        const bool needArt = _appMedia->post(u, millis());
        return r->reply(200, "application/json", needArt ? "{\"ok\":true,\"needArt\":true}"
                                                         : "{\"ok\":true,\"needArt\":false}");
    });

    // The cover the last post named: 48x48, one bit (MediaPane.h format, 576
    // hex), its CRC-32 as `h`. Refused when the bits do not match `h` or the
    // track has moved on.
    route("/api/app/art", HTTP_POST, [this](PsychicRequest* r) {
        if (!_appMedia) return r->reply(503, "text/plain", "not_built media");
        JsonDocument d(&PsramJson::instance());
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        const char* h = d["h"] | "";
        char* end = nullptr;
        const unsigned long hash = strtoul(h, &end, 16);
        if (strlen(h) != 8 || !end || *end) return r->reply(400, "text/plain", "param h");
        uint8_t bits[AppMediaSource::ART_BYTES];
        if (!MegaOpen::MediaPane::fromHex(d["bits"] | "", bits)) return r->reply(400, "text/plain", "param bits");
        if (!_appMedia->putArt((uint32_t)hash, bits)) return r->reply(409, "text/plain", "art_stale");
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    route("/api/media", HTTP_POST, [this](PsychicRequest* r) {
        if (!_ui.settings) return r->reply(503, "text/plain", "not_built display");
        JsonDocument d(&PsramJson::instance());
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");
        using MegaOpen::MediaPane;
        if (d["scroll"].is<int>() && !Catalog::arrowsKnown(d["scroll"].as<int>()))
            return r->reply(400, "text/plain", kScrollWhy);

        // `bits`: the Custom scene's picture (MediaPane.h format, 576 hex).
        // Checked whole, decoded straight into the staging buffer, stored, and
        // then the scene chosen.
        const bool hasBits = !d["bits"].isNull();
        if (hasBits) {
            const char* hex = d["bits"].as<const char*>();
            if (!MediaPane::fromHex(hex, nullptr))
                return r->reply(400, "text/plain", "param bits");
            if (!_ui.paneBits || !_ui.paneBits->edit([&](App::Ui::PaneBits& p) {
                    MediaPane::fromHex(hex, p.bits);
                    p.set = true;
                }))
                return r->reply(503, "text/plain", "busy");
            // The NVS write runs under the staging mutex (no second buffer).
            // The ui task only try-takes it (Staged::applyIfDirty), so it never
            // waits: the new picture lands on its first pass after this.
            bool saved = false;
            _ui.paneBits->read([&](const App::Ui::PaneBits& p) { saved = _cfg->savePaneBits(p.bits, sizeof(p.bits)); });
            if (!saved) return r->reply(500, "text/plain", "nvs_failed");
        }
        const char* paneName = hasBits ? "custom" : d["pane"].as<const char*>();
        if (paneName && !strcmp(paneName, "custom") && !hasBits) {
            bool drawn = false;
            if (_ui.paneBits) _ui.paneBits->read([&](const App::Ui::PaneBits& p) { drawn = p.set; });
            if (!drawn) return r->reply(409, "text/plain", "no_custom_pic");
        }

        // Every change is staged; the ui task applies the whole set on its next
        // pass (Staged<T>, §3.1). Nothing here touches the live screen or pane.
        bool save = false;
        bool splashNow = false;
        const bool ok = _ui.settings->edit([&](App::Ui::MediaSettings& m) {
            // The test fields: one poke, held until the source's next update.
            App::Ui::MediaPoke& pk = m.poke;
            pk.has = 0;
            if (d["title"].is<const char*>()) {
                strlcpy(pk.title, d["title"], sizeof(pk.title));
                pk.has |= App::Ui::MediaPoke::TITLE;
            }
            if (d["artist"].is<const char*>()) {
                strlcpy(pk.artist, d["artist"], sizeof(pk.artist));
                pk.has |= App::Ui::MediaPoke::ARTIST;
            }
            // The phone overwrites it on the next minute.
            if (d["clock"].is<const char*>()) {
                strlcpy(pk.clock, d["clock"], sizeof(pk.clock));
                pk.has |= App::Ui::MediaPoke::CLOCK;
            }
            if (d["state"].is<int>()) {
                pk.state = (uint8_t)(d["state"] | 0);
                pk.has |= App::Ui::MediaPoke::STATE;
            }
            if (d["elapsed"].is<int>()) {
                pk.elapsed = (uint16_t)(d["elapsed"] | 0);
                pk.has |= App::Ui::MediaPoke::ELAPSED;
            }
            if (d["duration"].is<int>()) {
                pk.duration = (uint16_t)(d["duration"] | 0);
                pk.has |= App::Ui::MediaPoke::DURATION;
            }
            if (pk.has) ++m.pokeSeq;

            if (d["cols"].is<int>()) m.cols = (uint8_t)(d["cols"] | MegaOpen::MediaScreen::COLS_DEFAULT);
            if (d["bar"].is<int>())  m.bar  = (uint8_t)(d["bar"] | 10);
            // `scrollMs` sets both rows, so it goes before the per-row values.
            if (d["scrollMs"].is<int>())
                m.scrollMsTitle = m.scrollMsArtist = (uint16_t)(d["scrollMs"] | 700);
            if (d["scrollMsTitle"].is<int>())  m.scrollMsTitle  = (uint16_t)(d["scrollMsTitle"] | 700);
            if (d["scrollMsArtist"].is<int>()) m.scrollMsArtist = (uint16_t)(d["scrollMsArtist"] | 1400);
            // Scroll arrows, panel encoding (the catalog's `arrows`, validated
            // above): 0x00 none, 0x07 up, 0x0B down, 0x03 both.
            if (d["scroll"].is<int>()) m.scroll = (uint8_t)(d["scroll"] | 0);
            // Persisted. Unknown names must not map to Fullscreen (it hides the pane).
            if (d["layout"].is<const char*>()) {
                m.layout = MegaOpen::MediaScreen::layoutFromName(d["layout"]);
                // Width follows the layout (8 beside the pane, 20 for the menu, 22
                // fullscreen) unless the same request carries `cols`.
                if (!d["cols"].is<int>()) m.cols = MegaOpen::MediaScreen::colsFor(m.layout);
                _cfg->s().mediaLayout = MegaOpen::MediaScreen::layoutName(m.layout);
                save = true;
            }
            // Style: persisted by name; unknown falls back to classic.
            if (d["style"].is<const char*>()) {
                m.style = MegaOpen::MediaScreen::styleFromName(d["style"]);
                _cfg->s().mediaStyle = MegaOpen::MediaScreen::styleName(m.style);
                save = true;
            }
            if (d["auto"].is<bool>()) {
                m.mediaAuto = d["auto"];
                _cfg->s().mediaScreen = m.mediaAuto;
                save = true;
            }
            // Pane scene or "off", persisted by name like the style. "auto"
            // starts the carousel without moving the live scene; any real scene
            // name stops it. sceneSeq makes the ui apply the choice once.
            // `bits` chooses the Custom scene, as `pane:"custom"` would.
            if (paneName) {
                m.paneAuto = !strcmp(paneName, "auto");
                if (m.paneAuto) {
                    _cfg->s().mediaPane = "auto";
                } else {
                    m.paneScene = MegaOpen::MediaPane::sceneFromName(paneName);
                    _cfg->s().mediaPane = MegaOpen::MediaPane::sceneName(m.paneScene);
                }
                ++m.sceneSeq;
                save = true;
            }
            // 5..600 s per stop.
            if (d["paneCycleS"].is<int>()) {
                m.paneCycleS = (uint16_t)constrain(d["paneCycleS"].as<int>(), 5, 600);
                _cfg->s().paneCycleS = m.paneCycleS;
                save = true;
            }
            // 1..6: beyond that it stops reading as motion at two images a second.
            if (d["paneSpeed"].is<int>()) {
                m.paneSpeed = (uint8_t)constrain(d["paneSpeed"].as<int>(), 1, 6);
                _cfg->s().paneSpeed = m.paneSpeed;
                save = true;
            }
            // Track splash: 0 = off, else 1..20 s.
            if (d["splashMs"].is<int>()) {
                const int ms = d["splashMs"].as<int>();
                m.splashMs = (uint16_t)(ms <= 0 ? 0 : constrain(ms, 1000, 20000));
                _cfg->s().trackSplashMs = m.splashMs;
                save = true;
            }
            // Clock push, independent of now-playing.
            if (d["autoTime"].is<bool>()) {
                m.autoTime = d["autoTime"];
                _cfg->s().autoTime = m.autoTime;
                save = true;
            }
            splashNow = d["splashNow"].as<bool>();
        });
        if (!ok) return r->reply(503, "text/plain", "busy");
        // NVS outside the staging mutex: a flash write can take tens of ms.
        if (save) _cfg->save();
        // Fire one now for the current track.
        if (splashNow && _ui.splashNow) _ui.splashNow();

        return r->reply(200, "application/json", "{\"ok\":true}");
    });

}
#endif
