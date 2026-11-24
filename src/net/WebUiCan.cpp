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

#if FEATURE_VEHICLE_CAN
// ---- CAN status + sniffer ------------------------------------------------------
void WebUi::routesCan()
{
    route("/api/can", HTTP_GET, [this](PsychicRequest* r) {
        JsonDocument d(&PsramJson::instance());
        // Listen-only on the car's bus; with the panel, the link's TX gate can
        // also have turned transmit off (nobody ACKs, nothing heard), which is
        // why busErr and twaiTx stop moving: say so here.
        bool txEnabled = false;
#if FEATURE_DISPLAY_CAN
        if (_panel) {
            const MegaOpen::PanelStatus p = _panel->read();
            txEnabled = !p.txGated;
            d["txGated"] = p.txGated;
            d["txGates"] = p.txGates;
        }
#endif
        d["txEnabled"] = txEnabled;
        d["listenOnly"] = Diag::CanBench::listenOnly();

        // Controller state: tells "no frames" from "frames but undecodable".
        // The loop's 100 ms sample: httpd does not call the driver.
        const Diag::CanStatus st = Diag::CanBench::status();
        if (st.installed) {
            static const char* const kState[] = {"stopped", "running", "bus-off", "recovering"};
            d["state"]    = kState[st.state <= 3 ? st.state : 0];
            d["txErr"]    = st.tec;
            d["rxErr"]    = st.rec;
            d["txFailed"] = st.txFailed;
            d["rxMissed"] = st.rxMissed;
            d["busErr"]   = st.busErr;
            d["arbLost"]  = st.arbLost;
            d["txQueued"] = st.msgsToTx;
        }
        // Our own transmit tally; the controller counters and the raw ring
        // don't show whether we ever called transmit.
        d["twaiTxOk"]   = st.twaiTxOk;
        d["twaiTxFail"] = st.twaiTxFail;
        d["twaiRx"]     = st.twaiRx;
        d["twaiMode"]   = st.mode == TWAI_MODE_LISTEN_ONLY ? "listen-only" : "normal";
        d["rxFrames"]  = _canRx ? *_canRx : 0;
        if (_sniffer) {
            d["ids"]      = _sniffer->count();
            d["frames"]   = _sniffer->frames();
            d["overflow"] = _sniffer->overflow();
            d["sniff"]    = _sniffer->enabled();
        }
        // The car task (Step 5): who decodes, and how much matched.
        if (App::Car::running()) {
            const App::Car::Stats c = App::Car::stats();
            JsonObject car = d["car"].to<JsonObject>();
            car["sources"]  = c.sources;
            car["frames"]   = c.frames;
            car["fps"]      = c.fps;
            car["decoded"]  = c.decodeHits;
            car["unknown"]  = c.decodeUnknown;
            car["dropped"]  = c.srcDropped;
            car["cmdDrops"] = c.cmdDrops;
        }
        d["rxPin"] = (int)Board::PIN_CAN_RX;
        d["txPin"] = (int)Board::PIN_CAN_TX;
        return replyDoc(r, d);
    });

    // Error Code Capture: type (bit/form/stuff/other), direction and frame
    // segment of bus errors. Bit error while sending in the ACK slot = our ACK
    // does not read back; form error in ACK delimiter/EOF while receiving = the
    // other node objects; stuff error in data = timing close but off.
    // Races the driver ISR for the latch, so it is a histogram, not a log.
    route("/api/can/ecc", HTTP_GET, [](PsychicRequest* r) {
        static const char* const kType[4] = {"bit", "form", "stuff", "other"};
        auto segName = [](uint8_t s) -> const char* {
            switch (s) {
                case 0x03: return "SOF";            case 0x02: return "ID28-21";
                case 0x06: return "ID20-18";        case 0x04: return "SRTR";
                case 0x05: return "IDE";            case 0x07: return "ID17-13";
                case 0x0F: return "ID12-5";         case 0x0E: return "ID4-0";
                case 0x0C: return "RTR";            case 0x0D: return "r1";
                case 0x09: return "r0";             case 0x0B: return "DLC";
                case 0x0A: return "data";           case 0x08: return "CRC";
                case 0x18: return "CRC delimiter";  case 0x19: return "ACK slot";
                case 0x1B: return "ACK delimiter";  case 0x1A: return "EOF";
                case 0x12: return "intermission";   case 0x11: return "active error flag";
                case 0x16: return "passive error flag";
                case 0x13: return "tolerate dominant"; case 0x17: return "error delimiter";
                case 0x1C: return "overload flag";  default:   return "?";
            }
        };
        uint16_t hist[256] = {0};
        for (uint16_t i = 0; i < 3000; ++i) {
            ++hist[TWAI.error_code_capture_reg.val & 0xFF];
            delayMicroseconds(40);
            if ((i & 0xFF) == 0xFF) delay(1);        // let the server breathe
        }
        JsonDocument d(&PsramJson::instance());
        JsonArray a = d["seen"].to<JsonArray>();
        for (uint16_t v = 0; v < 256; ++v) {
            if (!hist[v]) continue;
            JsonObject o = a.add<JsonObject>();
            o["raw"]   = v;
            o["count"] = hist[v];
            o["type"]  = kType[(v >> 6) & 3];
            o["dir"]   = (v & 0x20) ? "while RECEIVING" : "while SENDING";
            o["where"] = segName(v & 0x1F);
        }
        return replyDoc(r, d);
    });

    // Per-id table. `changed`: a bit per byte that differed since the baseline.
    route("/api/can/ids", HTTP_GET, [this](PsychicRequest* r) {
        if (!_sniffer) return r->reply(503, "text/plain", "not_built can");
        JsonDocument d(&PsramJson::instance());
        d["overflow"] = _sniffer->overflow();
        JsonArray a = d["ids"].to<JsonArray>();
        // copyAt: the panel task writes these rows while this runs on httpd.
        const uint8_t rows = _sniffer->count();
        for (uint8_t i = 0; i < rows; ++i) {
            Vehicle::CanSniffer::Entry e;
            if (!_sniffer->copyAt(i, e) || !e.used) continue;
            JsonObject o = a.add<JsonObject>();
            o["id"]      = e.id;
            o["count"]   = e.count;
            o["len"]     = e.len;
            o["changed"] = e.changed;
            char hex[24] = "";      // a DLC-0 frame writes nothing below
            int n = 0;
            for (uint8_t b = 0; b < e.len && b < 8; ++b)
                n += snprintf(hex + n, sizeof(hex) - n, "%02X", e.last[b]);
            o["data"] = hex;
        }
        return replyDoc(r, d);
    });

    route("/api/can/baseline", HTTP_POST, [this](PsychicRequest* r) {
        if (!_sniffer) return r->reply(503, "text/plain", "not_built can");
        // Applied by the panel task, which owns the table, before its next frame.
        _sniffer->requestBaseline();
        LOGI("can", "baseline marked — now do the thing you want to find");
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    route("/api/can/reset", HTTP_POST, [this](PsychicRequest* r) {
        if (!_sniffer) return r->reply(503, "text/plain", "not_built can");
        _sniffer->requestReset();   // the table's owner applies it
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // The per-id table on or off (?on=1|0). Off by default on the T-2CAN, where
    // the recorder is the tool and a busy car bus would pay a table search per
    // frame; on elsewhere. Not saved: every boot starts at the default.
    route("/api/can/sniff", HTTP_POST, [this](PsychicRequest* r) {
        if (!_sniffer) return r->reply(503, "text/plain", "not_built can");
        const bool on = paramStr(r, "on") == "1";
        _sniffer->setEnabled(on);
        LOGI("can", "sniffer %s", on ? "on" : "off");
        return r->reply(200, "application/json", on ? "{\"ok\":true,\"sniff\":true}"
                                                    : "{\"ok\":true,\"sniff\":false}");
    });
}
#endif

#if FEATURE_CAN2
// ---- CAN A -------------------------------------------------------------------
// The car's bus on the T-2CAN, listen-only always. Everything here is the car
// task's snapshot; a rate set here is handed to that task, which restarts the
// controller on its next wake.
void WebUi::routesCan2()
{
    route("/api/can2", HTTP_GET, [this](PsychicRequest* r) {
        if (!_can2) return r->reply(503, "text/plain", "not_built can2");
        const Mcp2518Source::Status c = _can2->read();
        static const char* const kAb[] = {"searching", "locked", "manual"};
        JsonDocument d(&PsramJson::instance());
        d["live"]          = c.live;
        d["listenOnly"]    = true;       // the only mode this firmware has
        d["autobaud"]      = kAb[c.state <= 2 ? c.state : 0];
        d["bitrate"]       = c.bps;      // listening at, now
        d["detected"]      = c.detected; // locked by the search, 0 = not yet
        d["setting"]       = _cfg->s().can2Bitrate;   // 0 = auto
        d["saved"]         = _cfg->s().can2Detected;  // tried first at boot
        d["fps"]           = c.fps;
        d["frames"]        = c.frames;
        d["fdDropped"]     = c.fdDropped;
        d["rtrDropped"]    = c.remoteDropped;
        d["intKicks"]      = c.intKicks;
        d["maskMaxUs"]     = c.maskMaxUs;
        d["hwOverflows"]   = c.hwOverflows;
        d["ringPeak"]      = c.ringPeak;
        d["rec"]           = c.trec & 0xFF;
        d["tec"]           = (c.trec >> 8) & 0xFF;
        d["errFlags"]      = (c.trec >> 16) & 0x3F;
        d["bdiag1"]        = c.bdiag1;
        d["restarts"]      = c.restarts;
        d["rounds"]        = c.cycles;
        d["rejected"]      = c.rejected;
        d["beginErr"]      = c.beginErr;
        return replyDoc(r, d);
    });

    // ?bitrate=125000|250000|500000|1000000 fixes the rate; 0 (or auto) goes
    // back to finding it. Saved; the car task applies it at once, no reboot.
    route("/api/can2", HTTP_POST, [this](PsychicRequest* r) {
        if (!_can2Rate) return r->reply(503, "text/plain", "not_built can2");
        const String v = paramStr(r, "bitrate");
        const uint32_t bps = (v == "auto" || !v.length()) ? 0u : (uint32_t)v.toInt();
        if (bps && !Bus::Autobaud::valid(bps))
            return r->reply(400, "text/plain", "param bitrate");
        _cfg->s().can2Bitrate = bps;
        _cfg->save();
        _can2Rate(bps);
        LOGI("can2", "rate %s", bps ? "set by hand" : "back to auto");
        return r->reply(200, "application/json", "{\"ok\":true}");
    });
}
#endif

// ---- the head unit's Raise link --------------------------------------------
//
// Profile messages with their bytes, arbitrary frames, keys, and what the head
// unit says back.
void WebUi::routesCanbox()
{
    // The emitter is the hu task's: its settings are read from the staged copy
    // (the newest edit) and previews are built on this handler's stack.
    route("/api/messages", HTTP_GET, [this](PsychicRequest* r) {
        Scratch<Canbox::HuConfig> cfgBox;   // ~450 B: not on httpd's 4 kB stack
        if (!cfgBox) return r->reply(503, "text/plain", "no_memory");
        Canbox::HuConfig& cfg = *cfgBox;
        if (!App::Hu::emitterCfg().peek(cfg)) return r->reply(503, "text/plain", "busy");
        JsonDocument d(&PsramJson::instance());
        JsonArray a = d["messages"].to<JsonArray>();
        const Canbox::Protocol& proto = Canbox::activeProtocol();
        for (uint8_t i = 0; i < proto.messageCount(); ++i) {
            const Canbox::MessageView mv = proto.message(i);
            JsonObject o = a.add<JsonObject>();
            o["id"]       = i;
            o["name"]     = mv.name;
            o["cmd"]      = mv.cmd;
            o["interval"] = mv.intervalMs;
            o["enabled"]  = i < Canbox::HuConfig::MAX_MESSAGES && cfg.msgOn[i];
            o["conf"]     = mv.conf;
            o["note"]     = mv.note;

            // The bytes it would send now; ready=false when none of its signals
            // is known (the emitter stays silent).
            uint8_t frame[Canbox::HU_FRAME_MAX];
            uint8_t len = 0;
            if (App::Hu::preview(i, frame, len)) {
                String hex; appendHex(hex, frame, len);
                o["ready"] = true;
                o["bytes"] = hex;
            } else {
                o["ready"] = false;
            }
        }
        return replyDoc(r, d);
    });

    // 409 when the message has no data, so nothing-sent never looks like success.
    // The hu task sends it (it owns the emitter and the UART's vehicle lane);
    // this handler waits up to 2 s for its answer.
    route("/api/messages/send", HTTP_POST, [this](PsychicRequest* r) {
        if (!_state || !_emitter) return r->reply(503, "text/plain", "not_built vehicle");
        if (!r->hasParam("id")) return r->reply(400, "text/plain", "param id");
        const uint8_t id = (uint8_t)atoi(paramStr(r, "id", "255").c_str());
        switch (App::Hu::sendNowWait(id)) {
            case App::Hu::SendNow::Sent:   break;
            case App::Hu::SendNow::NoData:
                return r->reply(409, "text/plain", "no_msg_data");
            default:
                return r->reply(503, "text/plain", "busy");
        }
        return r->reply(200, "application/json", "{\"ok\":true}");
    });

    // Switching a message on or off: the registry's `huMessages` table
    // (RAM only), POST /api/settings.

    // The last frames on the head-unit UART, both directions, with ASCII.
    // 'J' rows are bytes outside any frame: a different dialect shows up there
    // while rxFrames stays 0.
    route("/api/canbox/last", HTTP_GET, [this](PsychicRequest* r) {
        if (!_huDump) return r->reply(503, "text/plain", "not_built uart_ring");
        // ?in=1: the head unit's frames alone, kept apart from ours.
        ChunkOut out(r);
        _huDump(paramStr(r, "in") == "1", out);
        return out.finish();
    });

    // The Raise tuning (every value the sources disagree about, persisted:
    // they are resolved in the car, where every ignition cycle is a reboot)
    // is the registry's raise.* entries: GET/POST /api/settings.

    // Send any frame of the head-unit protocol: cmd + payload; its framer adds
    // the rest (Raise: the 0x2E header, length and checksum). No allow-list,
    // on purpose.
    route("/api/canbox/raw", HTTP_POST, [this](PsychicRequest* r) {
        if (!_tx) return r->reply(503, "text/plain", "not_built uart");
        if (!r->hasParam("cmd")) return r->reply(400, "text/plain", "param cmd");

        const uint32_t cmd = paramU32(r, "cmd", "", 16);
        if (cmd > 0xFF) return r->reply(400, "text/plain", "param cmd");

        uint8_t payload[Canbox::HU_FRAME_MAX];
        const int n = parseHex(paramStr(r, "data", "").c_str(), payload, sizeof(payload));
        if (n < 0) return r->reply(400, "text/plain", "param data");

        uint8_t frame[Canbox::HU_FRAME_MAX];
        const uint8_t flen = Canbox::activeProtocol().frame((uint8_t)cmd, payload, (uint8_t)n, frame);
        if (!flen) return r->reply(400, "text/plain", "too_long");

        // The hu task's vehicle lane, never the UART from here: httpd is on
        // core 0, where WiFi can preempt it while it holds the UART mutex. The
        // lane writes it into an empty FIFO, so it cannot delay an ack.
        if (!App::Hu::post(App::Hu::Kind::Raw, frame, flen))
            return r->reply(503, "text/plain", "queue_full");

        String hex; appendHex(hex, frame, flen);
        LOGI("hu", "raw tx: %s", hex.c_str());
        JsonDocument d(&PsramJson::instance()); d["ok"] = true; d["queued"] = true; d["bytes"] = hex;
        String out; serializeJson(d, out);
        if (!out.c_str()) return r->reply(503, "text/plain", "no_memory");
        return r->reply(202, "application/json", out.c_str());
    });

    // A key frame with a literal code, bypassing the keymap. For a knob code
    // (Raise 0x81-0x86) the second byte is a detent count, not a press state,
    // and there is no release.
    route("/api/canbox/key", HTTP_POST, [this](PsychicRequest* r) {
        if (!_keySink) return r->reply(503, "text/plain", "not_built uart");
        if (!r->hasParam("code")) return r->reply(400, "text/plain", "param code");
        const uint8_t code  = (uint8_t)paramU32(r, "code", "0", 16);
        const uint8_t state = (uint8_t)paramU32(r, "state", "1", 0);

        // A button needs its release or the head unit holds it; knobs have none.
        const bool release = !Canbox::activeProtocol().isKnobCode(code) && paramBool(r, "release", true) && state != 0;
        // The keys task sends it, after any key already queued.
        if (!App::KeyBus::postLiteral(code, state, release))
            return r->reply(503, "text/plain", "queue_full");
        return r->reply(202, "application/json", "{\"ok\":true,\"queued\":true}");
    });
}
