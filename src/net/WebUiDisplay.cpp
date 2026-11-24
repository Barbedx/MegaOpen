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
// ---- OEM display -------------------------------------------------------------
//
// Every render call here only enqueues; success means queued, not drawn.
void WebUi::routesDisplay()
{
    route("/api/display", HTTP_GET, [this](PsychicRequest* r) {
        JsonDocument doc(&PsramJson::instance());
        fillDisplay(doc.to<JsonObject>());
        return replyDoc(r, doc);
    });

    // The panel type and the board's role are registry entries (displayType,
    // panelRole: POST /api/settings, applied by a reboot): nothing is written
    // here.

    // One reply for every render route: 202 when the library took it (the
    // panel's answer comes later; /api/display reports lastResult), 503 when
    // it refused (PanelOps logs why).
    auto posted = [this](PsychicRequest* r, bool ok) {
        JsonDocument d(&PsramJson::instance());
        d["queued"] = ok;
        d["note"] = ok ? "queued for the display task; the result is reported in the log"
                       : "refused — nothing was drawn; the reason is in the log";
        String out; serializeJson(d, out);
        return r->reply(ok ? 202 : 503, "application/json", out.c_str());
    };

    route("/api/display/text", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        if (!r->hasParam("text")) return r->reply(400, "text/plain", "param text");
        const String t = r->getParam("text")->value();
        LOGD("disp", "text: %s", t.c_str());
        return posted(r, _ops->postText(t.c_str()));
    });

    route("/api/display/clock", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        if (!r->hasParam("time")) return r->reply(400, "text/plain", "param time");
        const String t = r->getParam("time")->value();
        const bool ok = _ops->postTime(t.c_str());
        // The ui task owns the clock's record (GET /api/media clockSrc): tell
        // it when the time is a real HHMM. It does not draw it again.
        const int hh = t.substring(0, 2).toInt(), mm = t.substring(2, 4).toInt();
        if (ok && _ui.manualClock && t.length() == 4 && isDigit(t[0]) && isDigit(t[1]) &&
            isDigit(t[2]) && isDigit(t[3]) && hh < 24 && mm < 60)
            _ui.manualClock((uint16_t)(hh << 8 | mm));
        return posted(r, ok);
    });

    route("/api/display/power", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        return posted(r, _ops->postPower(paramBool(r, "on", true)));
    });

    // Hand the glass back to automatic repaints now. `ms` sets the hold for later
    // draws; 0 = hold until this route is called.
    route("/api/display/release", HTTP_POST, [this](PsychicRequest* r) {
        if (!_ui.releaseGlass) return r->reply(503, "text/plain", "not_built display");
        if (r->hasParam("ms") && _ui.glassHoldMs)
            *_ui.glassHoldMs = paramU32(r, "ms", "10000", 10);
        _ui.releaseGlass();
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // Three rows of arbitrary ASCII: the general "draw anything" surface.
    route("/api/display/menu", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const String h  = paramStr(r, "header");
        const String r0 = paramStr(r, "row0");
        const String r1 = paramStr(r, "row1");
        uint8_t scroll = 0x0B;   // carminat::kScrollDown, the library's default
        if (r->hasParam("scroll")) {
            const uint32_t v = paramU32(r, "scroll", "11", 0);
            if (!Catalog::arrowsKnown((int)v)) return r->reply(400, "text/plain", kScrollWhy);
            scroll = (uint8_t)v;
        }
        return posted(r, _ops->postMenu(h.c_str(), r0.c_str(), r1.c_str(), scroll));
    });

    // Move the selection bar. Own RenderSlot, so it and a pending redraw never
    // replace each other.
    route("/api/display/highlight", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        return posted(r, _ops->postHighlight((uint8_t)atoi(paramStr(r, "row", "0").c_str())));
    });

    // icon / src / fmt exposed because they are still being reverse-engineered.
    route("/api/display/popup", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        if (paramBool(r, "hide")) return posted(r, _ops->postHidePopup());
        if (!r->hasParam("text")) return r->reply(400, "text/plain", "param text");
        const uint8_t icon = (uint8_t)paramU32(r, "icon", "0x55", 0);
        const uint8_t src  = (uint8_t)paramU32(r, "src",  "0xFF", 0);
        const uint8_t fmt  = (uint8_t)paramU32(r, "fmt",  "0x60", 0);
        const bool ok = _ops->postPopup(paramStr(r, "text").c_str(), icon, src, fmt);
        // `ms` arms the auto-hide (default 3 s, as the firmware's own popups);
        // ms=0 leaves it up until hide=1 or /api/display/clear.
        if (ok && _armPopup) {
            const uint32_t ms = paramU32(r, "ms", "3000", 0);
            if (ms) _armPopup(ms);
        }
        return posted(r, ok);
    });

    // The same path as a phone notification: fullscreen, sender + wrapped body.
    route("/api/display/notify", HTTP_POST, [this](PsychicRequest* r) {
        if (!_notify) return r->reply(503, "text/plain", "not_built display");
        _notify(paramStr(r, "title").c_str(), paramStr(r, "body").c_str());
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // Back to the media screen: ends a splash/notification, hides a popup,
    // releases the glass hold, resends text and pane.
    route("/api/display/clear", HTTP_POST, [this](PsychicRequest* r) {
        if (!_clear) return r->reply(503, "text/plain", "not_built display");
        _clear();
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    route("/api/display/confirm", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const String cap = paramStr(r, "caption");
        const String r0  = paramStr(r, "row0");
        const String r1  = paramStr(r, "row1");
        return posted(r, _ops->postConfirmBox(cap.c_str(), r0.c_str(), r1.c_str()));
    });

    // The mode 0x05 message box with a real button count (0..2; confirm is fixed
    // at one). `labels` is comma-separated, each truncated to 6 bytes. The
    // two-button form is 119 wire bytes, the longest message we send, and wraps
    // the ISO-TP counter to 0x20.
    route("/api/display/msgbox", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const String r0  = paramStr(r, "row0");
        const String r1  = paramStr(r, "row1");
        const String lab = paramStr(r, "labels");
        const uint8_t n  = (uint8_t)paramU32(r, "buttons", "1", 10);
        const uint8_t se = (uint8_t)paramU32(r, "selected", "0", 10);
        return posted(r, _ops->postMessageBox(r0.c_str(), r1.c_str(), lab.c_str(), n, se));
    });

    // `03 29 05 <n>`: select a message-box button. Not /api/display/highlight,
    // which is the list's `29 01 <rowtag>`.
    route("/api/display/selectbutton", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const uint8_t i = (uint8_t)paramU32(r, "index", "0", 10);
        return posted(r, _ops->postSelectButton(i));
    });

    // setText with the four header bytes exposed, including iconBank2. Icon
    // polarity is inverted (set bit = icon off, so "no icons" is 0x55). Defaults
    // are the library's captured values, so a bare call is a plain setText.
    route("/api/display/styled", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const String t = paramStr(r, "text");
        auto byteOf = [&](const char* k, uint32_t dflt) -> uint8_t {
            if (!r->hasParam(k)) return (uint8_t)dflt;
            return (uint8_t)paramU32(r, k, "0", 0);
        };
        return posted(r, _ops->postTextStyled(t.c_str(),
                                              byteOf("icon", 0x55),
                                              byteOf("src", 0xFF),
                                              byteOf("fmt", 0x60),
                                              byteOf("bank2", 0x55)));
    });

    // Any message on a registered function id (151 = the text, 1F1 = the nav
    // pane), the bytes as typed ("21 0B 00 ..."): the sweep tool for a screen
    // the library does not build yet (docs/nav-text.md). The library segments
    // it and copies the bytes at the call; past 119 bytes it goes through the
    // long-payload pool (320 at most).
    route("/api/display/raw", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const uint32_t id = paramU32(r, "id", "151", 16);
        if (!id || id > 0x7FF) return r->reply(400, "text/plain", "param id");
        struct RawBytes { uint8_t b[AFFA_MAX_EXTERNAL_PAYLOAD]; };
        Scratch<RawBytes> box;   // not on httpd's stack
        if (!box) return r->reply(503, "text/plain", "no_memory");
        const int n = parseHex(paramStr(r, "data", "").c_str(), box->b, sizeof(box->b));
        if (n <= 0) return r->reply(400, "text/plain", "param data");
        return posted(r, _ops->postRaw((uint16_t)id, box->b, (uint16_t)n));
    });

    // The nav-text sweep's verdicts («Пошук екрана навігації» on Сервіс,
    // docs/nav-text.md): one log line each, so a result reaches the pushed log
    // with the frame that made it. step: the card's id; verdict: yes / other /
    // no / lost; note: the owner's words, 60 bytes at most.
    route("/api/display/sweep", HTTP_POST, [](PsychicRequest* r) {
        const String step = paramStr(r, "step", ""), verdict = paramStr(r, "verdict", ""), note = paramStr(r, "note", "");
        if (!step.length() || step.length() > 16) return r->reply(400, "text/plain", "param step");
        if (verdict != "yes" && verdict != "other" && verdict != "no" && verdict != "lost")
            return r->reply(400, "text/plain", "param verdict");
        LOGI("disp", "sweep %s = %s%s%.60s", step.c_str(), verdict.c_str(), note.length() ? " " : "", note.c_str());
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // The three-row info list: three messages on the wire, one per row, not
    // coalesced. No hide command is known; draw the next screen instead.
    route("/api/display/info", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const String l1 = paramStr(r, "l1");
        const String l2 = paramStr(r, "l2");
        const String l3 = paramStr(r, "l3");
        return posted(r, _ops->postInfoPopup(l1.c_str(), l2.c_str(), l3.c_str()));
    });

    // No hide: a fullscreen is not an overlay, setText() over it replaces it
    // (measured on the panel). `02 54 03` is popup?hide=1.
    route("/api/display/fullscreen", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        const String l1 = paramStr(r, "l1");
        const String l2 = paramStr(r, "l2");
        const String l3 = paramStr(r, "l3");
        return posted(r, _ops->postFullscreen(l1.c_str(), l2.c_str(), l3.c_str()));
    });

    // Inject a key as if the panel sent it (KeySource::Local, see PanelOps);
    // same path as a key decoded off the wire.
    //
    // wire=1 instead puts the key frame on CAN B as the panel would
    // (KeySource::Wire): for driving a FACTORY radio that shares the bus with
    // us, to test it. In the usual radio role it means nothing (nobody but us
    // listens for panel keys) and the frame carries the panel's own id, so with
    // the real panel attached it is a phantom press other modules may act on.
    // The library sends it only while the link is up and the opening is done
    // (or passive); without that it would only be refused in the log, so 409.
    route("/api/display/key", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        if (!r->hasParam("code")) return r->reply(400, "text/plain", "param code");
        const uint16_t code = (uint16_t)paramU32(r, "code", "0", 0);
        const bool wire = paramBool(r, "wire"), hold = paramBool(r, "hold");
        if (wire && hold && !Keys::AffaMap::wireHold((affa::Key)code))
            return r->reply(400, "text/plain", "held_no_wire");
        if (wire) {
            const MegaOpen::PanelStatus st = _panel ? _panel->read() : MegaOpen::PanelStatus{};
            if (!st.synced && !st.passive)
                return r->reply(409, "text/plain", "panel_down");
        }
        return posted(r, _ops->postKey(code, hold, wire));
    });

    // Re-run the handshake: begin() clears both FSMs and cancels the queue. The
    // library posts it to the panel task, in order with the renders posted
    // before it (they complete Cancelled).
    route("/api/display/resync", HTTP_POST, [this](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        if (!_ops->postResync()) return r->reply(503, "text/plain", "queue_full");
        LOGI("hu", "resync requested");
        return r->reply(200, "application/json",
                        "{\"ok\":true,\"note\":\"handshake restarting; watch registered\"}");
    });

    // Drop everything queued but not started; the transfer in flight completes.
    route("/api/display/abort", HTTP_POST, [this, posted](PsychicRequest* r) {
        if (!_ops) return r->reply(503, "text/plain", "not_built display");
        return posted(r, _ops->postAbort());
    });
}

// ---- the stalk mapping --------------------------------------------------------
//
// Sixteen rows: eight keys x {click, hold}. Each has an action (seen by every
// sink) and an optional literal RAV4 code (0 = profile default for the action).
void WebUi::routesAffaMap()
{
    // The map the keys task looks keys up in is staged: the page reads and
    // writes the newest edit, and the keys task swaps it in at its next pass.
    route("/api/affamap", HTTP_GET, [this](PsychicRequest* r) {
        if (!_affaMap || !_st.affa) return r->reply(503, "text/plain", "not_built display");
        char last[160];     // worst case ~130: 10-digit seq, longest key and action, all five dests
        keyLastJson(last, sizeof(last));

        // The wiring check polls this every 300 ms: one fixed buffer, no heap.
        if (paramBool(r, "live")) {
            char out[288];
            const uint16_t u = _affaSrc ? _affaSrc->lastUnmapped() : 0;
            snprintf(out, sizeof(out),
                     "{\"last\":%s,\"unmapped\":{\"count\":%u,\"last\":%u,\"hold\":%s,\"known\":%s},"
                     "\"events\":%u}",
                     last, (unsigned)(_affaSrc ? _affaSrc->unmapped() : 0), (unsigned)u,
                     _affaSrc && _affaSrc->lastUnmappedHold() ? "true" : "false",
                     Keys::AffaMap::indexOf((affa::Key)u) < Keys::AffaMap::KEYS ? "true" : "false",
                     (unsigned)(_affaSrc ? _affaSrc->events() : 0));
            return r->reply(200, "application/json", out);
        }

        Keys::AffaMap map;
        if (!_st.affa->peek(map)) return r->reply(503, "text/plain", "busy");
        JsonDocument d(&PsramJson::instance());

        JsonArray rows = d["rows"].to<JsonArray>();
        for (uint8_t k = 0; k < Keys::AffaMap::KEYS; ++k) {
            for (uint8_t h = 0; h < 2; ++h) {
                const uint8_t i = Keys::AffaMap::rowIndex(k, h != 0);
                const Keys::AffaMap::Row& row = map.row(i);
                JsonObject o = rows.add<JsonObject>();
                o["index"]  = i;
                o["key"]    = Keys::AffaMap::keyName(k);
                o["wire"]   = (uint16_t)Keys::AffaMap::keyAt(k);
                o["hold"]   = (h != 0);
                o["action"] = Keys::name(row.action);
                o["code"]   = row.code;
                o["asLong"] = row.asLong;
                // What actually goes on the wire: override, else profile default.
                const Canbox::Protocol& P = Canbox::activeProtocol();
                const uint8_t eff = row.code ? row.code : P.keyCodeFor(row.action);
                o["effective"] = eff;
                o["effectiveName"] = P.keyCodeName(eff);
            }
        }

        if (_affaSrc) {
            JsonObject u = d["unmapped"].to<JsonObject>();
            u["count"] = _affaSrc->unmapped();
            u["last"]  = _affaSrc->lastUnmapped();
            u["hold"]  = _affaSrc->lastUnmappedHold();
            // known=false: a code outside the eight keys (new stalk); true: an emptied row.
            u["known"] = Keys::AffaMap::indexOf((affa::Key)_affaSrc->lastUnmapped())
                             < Keys::AffaMap::KEYS;
            d["events"] = _affaSrc->events();
        }
        d["last"] = serialized((const char*)last);
        return replyDoc(r, d);
    });

    route("/api/affamap", HTTP_POST, [this](PsychicRequest* r) {
        if (!_affaMap || !_st.affa) return r->reply(503, "text/plain", "not_built display");
        JsonDocument d(&PsramJson::instance());
        if (deserializeJson(d, r->body())) return r->reply(400, "text/plain", "bad_json");

        uint16_t n = 0;
        Keys::AffaMap saved;
        const bool ok = _st.affa->edit([&](Keys::AffaMap& m) {
            for (JsonObjectConst o : d["rows"].as<JsonArrayConst>()) {
                const uint8_t i = o["index"] | 0xFF;
                if (i >= Keys::AffaMap::ROWS) continue;
                Keys::AffaMap::Row row;
                row.action = Keys::fromName(o["action"] | "None");
                row.code   = (uint8_t)(o["code"] | 0);
                row.asLong = o["asLong"] | false;
                m.setRow(i, row);
                ++n;
            }
            saved = m;
        });
        if (!ok) return r->reply(503, "text/plain", "busy");
        App::KeyBus::kick();
        _cfg->saveAffaMap(saved);
        LOGI("cfg", "stalk mapping saved (%u rows)", (unsigned)n);
        JsonDocument res(&PsramJson::instance()); res["ok"] = true; res["applied"] = n;
        return replyDoc(r, res);
    });

    route("/api/affamap/defaults", HTTP_POST, [this](PsychicRequest* r) {
        if (!_affaMap || !_st.affa) return r->reply(503, "text/plain", "not_built display");
        Keys::AffaMap saved;
        if (!_st.affa->edit([&](Keys::AffaMap& m) { m.loadDefaults(); saved = m; }))
            return r->reply(503, "text/plain", "busy");
        App::KeyBus::kick();
        _cfg->saveAffaMap(saved);
        LOGI("cfg", "stalk mapping reset to defaults");
        return r->reply(200, "application/json", "{\"ok\":true}");
    });
}

// {seq,key,wire,hold,action,dests[]}: the last panel key as the keys task
// resolved it, or {seq:0} before the first. A copy of the snapshot only.
void WebUi::keyLastJson(char* out, size_t cap) const
{
    Keys::PanelKeyLast k;
    const uint32_t seq = _keyLast ? _keyLast->read(k) : 0;
    if (!seq) { snprintf(out, cap, "{\"seq\":0}"); return; }
    char dests[48] = "";
    size_t n = 0;
    for (uint8_t i = 0; i < Keys::DEST_COUNT && n < sizeof(dests); ++i)
        if (k.dests & (1u << i))
            n += snprintf(dests + n, sizeof(dests) - n, "%s\"%s\"", n ? "," : "", Keys::DEST_IDS[i]);
    snprintf(out, cap,
             "{\"seq\":%u,\"key\":\"%s\",\"wire\":%u,\"hold\":%s,\"action\":\"%s\",\"dests\":[%s]}",
             (unsigned)seq, k.key < Keys::AffaMap::KEYS ? Keys::AffaMap::keyName(k.key) : "",
             (unsigned)k.wire, k.hold ? "true" : "false",
             Keys::name((Keys::Action)k.action), dests);
}

#endif

#if FEATURE_DISPLAY_CAN
// The body of /api/display.
void WebUi::fillDisplay(JsonObject d)
{
        d["type"]     = _cfg->s().displayType.c_str();
        d["canRx"]    = _canRx ? *_canRx : 0;
        // The panel task's copy, published after every poll(): the library
        // itself is never called from here (httpd).
        const MegaOpen::PanelStatus ps = _panel ? _panel->read() : MegaOpen::PanelStatus{};
        d["attached"] = ps.attached;
        if (ps.attached) {
            const affa::SyncState s = (affa::SyncState)ps.syncState;
            d["sync"]       = (uint8_t)s;
            JsonObject fl = d["syncFlags"].to<JsonObject>();
            fl["failed"]    = affa::hasFlag(s, affa::SyncState::Failed);
            fl["peerAlive"] = affa::hasFlag(s, affa::SyncState::PeerAlive);
            fl["start"]     = affa::hasFlag(s, affa::SyncState::Start);
            fl["funcsReg"]  = affa::hasFlag(s, affa::SyncState::FuncsReg);
            d["synced"]     = ps.synced;
            d["registered"] = ps.registered;
            d["passive"]    = ps.passive;
            d["role"]       = MegaOpen::PanelRoles::id((MegaOpen::PanelRole)ps.role);
            d["busy"]       = ps.busy;
            d["queued"]     = ps.queued;
            d["lastResult"] = affa::resultName((affa::Result)ps.lastResult);
            JsonObject f = d["supports"].to<JsonObject>();
            f["text"]       = (ps.features & MegaOpen::PF_TEXT) != 0;
            f["time"]       = (ps.features & MegaOpen::PF_TIME) != 0;
            f["power"]      = (ps.features & MegaOpen::PF_POWER) != 0;
            f["menu"]       = (ps.features & MegaOpen::PF_MENU) != 0;
            f["popup"]      = (ps.features & MegaOpen::PF_POPUP) != 0;
            f["fullscreen"] = (ps.features & MegaOpen::PF_FULLSCREEN) != 0;
            f["confirm"]    = (ps.features & MegaOpen::PF_CONFIRM) != 0;
            f["info"]       = (ps.features & MegaOpen::PF_INFO) != 0;
        }
        // Bus counters for the panel's TwaiLink are on /api/can.
        if (ps.attached) {
            // Where the opening got to, and why the last session ended (see the
            // phase log in src/app/UiTask.cpp).
            d["phase"]       = affa::phaseName((affa::Phase)ps.phase);

            // Last RenderSlot drawn, tracked by the library. The pane never shows
            // (showNavBitmap() uses RenderSlot::None); `popup` means the popup was
            // drawn last, not that the screen under it is gone.
            static const char* const kSlot[] = {
                "text", "clock", "menu", "highlight", "popup",
                "fullscreen", "confirm", "info", "control", "none"};
            const uint8_t sl = ps.lastRendered;
            d["lastRendered"]   = kSlot[sl < 10 ? sl : 9];
            d["lastRenderedMs"] = ps.lastRenderedMs;

            // Time left before automatic repaints resume after a posted screen;
            // check this first when the media screen "stopped". 0xFFFFFFFF = never.
            // The ui task's copy, so no pointers.
            if (_ui.status) {
                const uint32_t nowMs = millis();
                const uint32_t held  = _ui.status->read().glassHeldUntilMs;
                const uint32_t hold  = _ui.glassHoldMs ? *_ui.glassHoldMs : 0;
                d["glassHoldMs"] = hold;
                d["glassHeldMs"] =
                    !held                                ? 0
                    : !hold                              ? 0xFFFFFFFFu
                    : ((int32_t)(nowMs - held) < 0)      ? (held - nowMs)
                                                         : 0;
            }

            d["sessionsLost"] = ps.sessionsLost;
            d["lastLoss"]    = affa::lossReasonName((affa::LossReason)ps.lastLoss);
            d["linkFlaps"]      = ps.linkFlaps;
            d["linkRecoveries"] = ps.linkRecoveries;
            d["linkDownMs"]     = ps.linkDownMs;
            // The panel task's pump and its TX gate (src/app/PanelOwner.h).
            d["foreignPolls"]  = ps.foreignPolls;
            d["pollLateMaxUs"] = ps.pollLateMaxUs;
            d["txGated"]       = ps.txGated;
            d["txGates"]       = ps.txGates;
            const uint32_t inst = Diag::CanBench::status().installs;
            d["reinstalls"]    = inst ? inst - 1 : 0;
            d["failStreak"]    = ps.failStreak;
        }
        if (_ops) {
            d["rendersPosted"]  = _ops->posted();
            // Renders refused at the call (the log says why), never drawn.
            d["rendersRefused"] = _ops->refused();
        }
}
#else
void WebUi::fillDisplay(JsonObject d) { d["attached"] = false; }
#endif
