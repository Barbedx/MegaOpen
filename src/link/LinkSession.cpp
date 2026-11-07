#include "LinkSession.h"
#if FEATURE_APP_LINK
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include "lwip/sockets.h"
#include "../Version.h"
#include "../app/CarTask.h"
#include "../utils/LogBus.h"
#include "../utils/PsHeap.h"
#include "../utils/WallClock.h"

namespace Link
{
    // The app sent a frame lately (it pings every second).
    bool Session::hostLive() const { return _anyRx && millis() - _lastRx < ALIVE_MS; }

    bool Session::alive() const { return _up && hostLive(); }

    bool Session::busy() const
    {
        for (const auto& c : _ch) if (c.fd >= 0) return true;
        return false;
    }

    // ---- out ----------------------------------------------------------------
    // A host that stops reading is given TX_GIVE_UP_MS (a flash write stalls
    // the board itself for half a second, and a frame given up half-written
    // costs the app its response). An app that has gone quiet (no frame in
    // ALIVE_MS: it died, or was closed, with the transport still up) reads
    // nothing: waiting the full 3 s a frame for it kept this task from reading
    // the next app's HELLO (the car's "USB: failed, write", boot 192,
    // 2026-10-28). Its frames wait TX_GONE_MS, then are dropped.
    bool Session::send(uint8_t type, uint8_t flags, uint16_t chan, const uint8_t* p, size_t len)
    {
        const size_t n = encode(type, flags, chan, p, len, _tx);
        if (!n || !_up) return false;
        switch (_pipe.write(_tx, n, hostLive() ? TX_GIVE_UP_MS : TX_GONE_MS, _st.txWaitMax)) {
            case Pipe::Tx::Ok:
                ++_st.txFrames;
                _st.txBytes += n;
                return true;
            case Pipe::Tx::Cut:     ++_st.cut;     return false;
            case Pipe::Tx::Dropped: ++_st.dropped; return false;
        }
        return false;
    }

    bool Session::sendText(uint8_t type, uint16_t chan, const char* s)
    {
        return send(type, FIN, chan, (const uint8_t*)s, strlen(s));
    }

    // ---- channels -------------------------------------------------------------
    void Session::closeChan(uint16_t c)
    {
        if (c >= CHANS || _ch[c].fd < 0) return;
        ::close(_ch[c].fd);
        _ch[c].fd = -1;
    }

    void Session::fail(uint16_t c, const char* why)
    {
        closeChan(c);
        ++_st.fails;
        sendText(HTTP_FAIL, c, why);
    }

    void Session::closeAll()
    {
        for (uint16_t c = 0; c < CHANS; ++c) closeChan(c);
    }

    bool Session::openChan(uint16_t c)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) return false;
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(80);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // A request body (an OTA upload) waits on the server reading it: at
        // most this long a send, then the channel fails.
        struct timeval tv = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        const uint32_t c0 = millis();
        const int cr = ::connect(fd, (struct sockaddr*)&a, sizeof(a));
        if (millis() - c0 > _st.connectMax) _st.connectMax = millis() - c0;
        if (cr != 0) {
            ::close(fd);
            return false;
        }
        _ch[c].fd = fd;
        _ch[c].lastMs = millis();
        ++_st.reqs;
        return true;
    }

    void Session::onRequest(const Frame& f)
    {
        const uint16_t c = f.chan;
        if (c >= CHANS) { sendText(HTTP_FAIL, c, "busy"); ++_st.fails; return; }
        if (_ch[c].fd < 0 && !openChan(c)) { fail(c, "connect"); return; }
        size_t at = 0;
        while (at < f.len) {
            const uint32_t s0 = millis();
            const int w = ::send(_ch[c].fd, f.payload + at, f.len - at, 0);
            if (millis() - s0 > _st.sockSendMax) _st.sockSendMax = millis() - s0;
            if (w <= 0) { fail(c, "send"); return; }
            at += (size_t)w;
        }
        _ch[c].lastMs = millis();
    }

    // Server to app, as much as there is now; the server closing is FIN.
    void Session::pump(uint32_t now)
    {
        for (uint16_t c = 0; c < CHANS; ++c) {
            if (_ch[c].fd < 0) continue;
            for (int i = 0; i < 4; ++i) {   // up to 4 kB a pass, then the others
                const int r = ::recv(_ch[c].fd, _body, sizeof(_body), MSG_DONTWAIT);
                if (r > 0) {
                    _ch[c].lastMs = now;
                    // Cut or not sent: the app hears 91 (if the link is back), never silence.
                    if (!send(HTTP_RESP, 0, c, _body, (size_t)r)) { fail(c, "send"); break; }
                    continue;
                }
                if (r == 0) {
                    send(HTTP_RESP, FIN, c, nullptr, 0);
                    closeChan(c);
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    fail(c, "recv");
                } else if ((int32_t)(now - _ch[c].lastMs) > (int32_t)IDLE_MS) {   // signed: a request may have opened it after `now`
                    fail(c, "timeout");
                }
                break;
            }
        }
    }

    // ---- the app's internet -----------------------------------------------
    void Session::netFail(const char* why)
    {
        snprintf(_netWhy, sizeof(_netWhy), "%s", why);
        _netSt = NetSt::Fail;
    }

    // applink, every pass: the request out, the credit back, a cancel.
    void Session::netService()
    {
        if (_netCancel) {
            _netCancel = false;
            send(NET_CANCEL, FIN, _netId, nullptr, 0);
        }
        if (_netSt == NetSt::Send) {
            bool ok = send(NET_REQ, _netBodyLen ? 0 : FIN, _netId, (const uint8_t*)_netReq, _netReqLen);
            for (size_t at = 0; ok && at < _netBodyLen;) {
                const size_t n = _netBodyLen - at < MAX_PAYLOAD ? _netBodyLen - at : MAX_PAYLOAD;
                ok = send(NET_BODY, at + n >= _netBodyLen ? FIN : 0, _netId, _netBody + at, n);
                at += n;
            }
            if (ok) _netSt = NetSt::Head;
            else netFail("send");
        }
        if (_netSt == NetSt::Body) {
            const uint32_t taken = _netTaken;
            if (taken - _netAcked >= NET_ACK_STEP) {
                const uint8_t p[4] = {(uint8_t)taken, (uint8_t)(taken >> 8), (uint8_t)(taken >> 16), (uint8_t)(taken >> 24)};
                if (send(NET_ACK, FIN, _netId, p, 4)) _netAcked = taken;
            }
        }
    }

    void Session::onNet(const Frame& f)
    {
        if (f.chan != _netId) return;     // a transfer given up on
        if (f.type == NET_FAIL) {
            if (_netSt == NetSt::Head || _netSt == NetSt::Body) {
                char w[sizeof(_netWhy)];
                const size_t n = f.len < sizeof(w) - 1 ? f.len : sizeof(w) - 1;
                memcpy(w, f.payload, n);
                w[n] = 0;
                netFail(w);
            }
            return;
        }
        if (f.type == NET_HEAD) {
            if (_netSt != NetSt::Head) return;
            JsonDocument d;
            if (deserializeJson(d, f.payload, f.len)) { netFail("head"); return; }
            _netHead.status = d["status"] | 0;
            _netHead.len = d["len"] | (int64_t)-1;
            snprintf(_netHead.url, sizeof(_netHead.url), "%s", d["url"] | "");
            _netSt = NetSt::Body;
            return;
        }
        if (f.type == NET_DATA) {
            if (_netSt != NetSt::Body) return;
            if (f.len && xStreamBufferSend(_netSb, f.payload, f.len, 0) < f.len) {
                netFail("window");     // the app sent past its credit
                return;
            }
            if (f.flags & FIN) _netEof = true;
        }
    }

    // ---- control ------------------------------------------------------------
    void Session::hello(const Frame& f)
    {
        {
            JsonDocument a;
            _appNet = !deserializeJson(a, f.payload, f.len) && (a["net"] | 0) != 0;
            if (a["time"].is<int64_t>()) WallClock::fromApp(a["time"].as<int64_t>(), tag());
        }
        if (_netSt != NetSt::Idle) netFail("new_app");
        JsonDocument d;
        d["proto"] = 1;
        d["fw"] = MEGAOPEN_VERSION;
        d["build"] = MegaOpen::buildId();
        d["name"] = _name;
        d["chans"] = CHANS;
        d["upMs"] = millis();
        d["via"] = tag();
        char out[256];
        const size_t n = serializeJson(d, out, sizeof(out));
        // Alerts from now on; what came before the app was listening is not said.
        static App::Car::AlertEntry none[1];
        App::Car::alertsSince(0, none, 0, _evSeq);
        closeAll();     // a new HELLO is a new app: its old requests are gone
        if (!_hello) LOGI("link", "%s: the app said HELLO (net %s)", tag(), _appNet ? "offered" : "no");
        _hello = true;
        send(HELLO_ACK, FIN, 0, (const uint8_t*)out, n);
    }

    void Session::onFrame(const Frame& f)
    {
        ++_st.rxFrames;
        _lastRx = millis();
        _anyRx = true;
        switch (f.type) {
            case HELLO:      hello(f); break;
            case PING:       send(PONG, FIN, f.chan, f.payload, f.len); break;
            case HTTP_REQ:   onRequest(f); break;
            case HTTP_CLOSE: closeChan(f.chan); break;
            case NET_HEAD:
            case NET_DATA:
            case NET_FAIL:   onNet(f); break;
            default: break;        // an app newer than us: ignored
        }
    }

    void Session::events(uint32_t now)
    {
        // An app gone quiet: the alerts wait (_evSeq stays) for it or the next HELLO.
        if (!_hello || !hostLive() || now - _evAt < EVENT_MS) return;
        _evAt = now;
        static App::Car::AlertEntry list[App::Car::ALERT_LOG];   // applink's alone
        uint32_t last = 0;
        const uint8_t n = App::Car::alertsSince(_evSeq, list, App::Car::ALERT_LOG, last);
        if (last < _evSeq) _evSeq = 0;   // cannot happen without a reboot; be safe
        for (uint8_t i = 0; i < n; ++i) {
            JsonDocument d;
            d["t"] = "alert";
            d["seq"] = list[i].seq;
            d["sig"] = list[i].rule;
            d["value"] = list[i].value;
            d["text"] = list[i].text;
            char out[192];
            const size_t len = serializeJson(d, out, sizeof(out));
            if (send(EVENT, FIN, 0, (const uint8_t*)out, len)) ++_st.events;
            _evSeq = list[i].seq;
        }
    }

    void Session::down()
    {
        closeAll();
        _parser.reset();
        if (_hello) LOGI("link", "%s: the app is gone", tag());
        _hello = false;
        _appNet = false;
        _anyRx = false;
        if (_netSt != NetSt::Idle) netFail("unplugged");
    }

    void Session::service(uint32_t now)
    {
        const bool up = _pipe.up();
        if (up != _up) {
            _up = up;
            if (!up) down();
        }
        if (up) {
            size_t r;
            while ((r = _pipe.read(_rx, sizeof(_rx))) > 0) {
                _st.rxBytes += r;
                _parser.feed(_rx, r, frameCb, this);
            }
            pump(now);
            events(now);
            netService();
        }
        uint8_t open = 0;
        for (const auto& c : _ch) open += c.fd >= 0;
        _st.open = open;
        _st.up = up;
        _st.hello = _hello;
        _st.bad = _parser.bad();
    }

    void Session::stats(SessionStats& out) const
    {
        out = _st;
        out.alive = alive();
        out.lastRxAgoMs = _anyRx ? millis() - _lastRx : UINT32_MAX;
    }

    // ---- the app's internet (any task but applink) --------------------------
    bool Session::netOpen(const char* method, const char* url, const char* const* headers,
                          const uint8_t* body, size_t len, uint32_t timeoutMs, NetHead& head,
                          const char*& why, void (*wake)())
    {
        if (!netAvailable()) { why = "no_app"; return false; }
        if (_netSt != NetSt::Idle) { why = "busy"; return false; }
        if (!_netSb) {
            // The body's buffer in PSRAM: as much as the app may send unasked.
            uint8_t* store = (uint8_t*)PsHeap::malloc(NET_WINDOW + 1);
            if (!store) { why = "no_memory"; return false; }
            _netSb = xStreamBufferCreateStatic(NET_WINDOW, 1, store, &_netSbStruct);
        }
        {
            JsonDocument d;
            d["m"] = method;
            d["url"] = url;
            JsonObject h = d["h"].to<JsonObject>();
            for (const char* const* p = headers; p && p[0] && p[1]; p += 2) h[p[0]] = p[1];
            d["len"] = len;
            const size_t n = measureJson(d);
            if (n >= sizeof(_netReq)) { why = "too_long"; return false; }
            _netReqLen = serializeJson(d, _netReq, sizeof(_netReq));
        }
        // The new number first: a late frame of the last transfer is refused
        // from here on, before its buffer is reused.
        _netId = (uint16_t)(_netId + 1) ? (uint16_t)(_netId + 1) : 1;
        xStreamBufferReset(_netSb);
        _netEof = false;
        _netTaken = 0;
        _netAcked = 0;
        _netWhy[0] = 0;
        _netBody = body;
        _netBodyLen = len;
        _netSt = NetSt::Send;
        if (wake) wake();
        const uint32_t t0 = millis();
        while (_netSt == NetSt::Send || _netSt == NetSt::Head) {
            // Never back out while the body is being sent: it is the caller's.
            if (_netSt == NetSt::Head && millis() - t0 > timeoutMs) {
                _netCancel = true;
                _netSt = NetSt::Idle;
                why = "timeout";
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (_netSt == NetSt::Fail) {
            static char w[sizeof(_netWhy)];   // one requester at a time (NetJob)
            memcpy(w, _netWhy, sizeof(w));
            why = w;
            _netSt = NetSt::Idle;
            return false;
        }
        head = _netHead;
        return true;
    }

    int Session::netRead(uint8_t* buf, size_t n, uint32_t timeoutMs, void (*wake)())
    {
        const uint32_t t0 = millis();
        for (;;) {
            if (_netSt != NetSt::Body) return -1;
            const size_t r = xStreamBufferReceive(_netSb, buf, n, pdMS_TO_TICKS(50));
            if (r) {
                _netTaken += r;
                if (wake) wake();   // the credit goes back sooner
                return (int)r;
            }
            if (_netEof && xStreamBufferIsEmpty(_netSb)) return 0;
            if (millis() - t0 > timeoutMs) return -1;
        }
    }

    void Session::netClose(void (*wake)())
    {
        if (_netSt == NetSt::Body && !(_netEof && xStreamBufferIsEmpty(_netSb))) _netCancel = true;
        _netSt = NetSt::Idle;
        if (wake) wake();
    }
}
#endif
