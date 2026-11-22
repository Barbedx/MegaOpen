#include "LinkSession.h"
#if FEATURE_APP_LINK
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include <fcntl.h>
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
        return !txIdle();
    }

    // ---- out ----------------------------------------------------------------
    // Nothing here waits (R2). The frame being written keeps its offset and
    // goes on next pass; the give-up is counted across passes. A host that
    // stops reading is given TX_GIVE_UP_MS (a flash write stalls the board
    // itself for half a second, and a frame given up half-written costs the
    // app its response). An app that has gone quiet (no frame in ALIVE_MS:
    // it died, or was closed, with the transport still up) reads nothing:
    // the full 3 s a frame for it kept this task from reading the next app's
    // HELLO (the car's "USB: failed, write", boot 192, 2026-10-28). Its
    // frames wait TX_GONE_MS, then are dropped.
    bool Session::fill(Out& o, uint8_t type, uint8_t flags, uint16_t chan, const uint8_t* p, size_t len)
    {
        size_t n = encode(type, flags, chan, p, len, o.buf);
        if (!n) return false;
        const size_t pad = _pipe.pad(n);   // the USB filler: at most 1, buf has MAX_FRAME + 1
        memset(o.buf + n, 0, pad);
        n += pad;
        o.n = (uint16_t)n;
        o.at = 0;
        o.type = type;
        o.flags = flags;
        o.chan = chan;
        return true;
    }

    bool Session::control(uint8_t type, uint8_t flags, uint16_t chan, const uint8_t* p, size_t len)
    {
        if (!_up) return false;
        if (_ctlN == CTL_SLOTS) { ++_st.dropped; return false; }
        Out& o = _ctl[(_ctlHead + _ctlN) % CTL_SLOTS];
        if (!fill(o, type, flags, chan, p, len)) return false;
        ++_ctlN;
        return true;
    }

    bool Session::controlText(uint8_t type, uint16_t chan, const char* s)
    {
        return control(type, FIN, chan, (const uint8_t*)s, strlen(s));
    }

    // No progress since _txSince: given up yet? True when it was.
    bool Session::txStalled(uint32_t t)
    {
        const uint32_t w = t - _txSince;
        if (w > _st.txWaitMax) _st.txWaitMax = w;
        const bool talking = _pipe.cutWhileTalking() && _st.rxFrames != _txRx && w > TX_RX_WAITING_MS;
        if (w <= (hostLive() ? TX_GIVE_UP_MS : TX_GONE_MS) && !talking) return false;
        txDone(false);
        return true;
    }

    // The frame being written is out (ok) or given up.
    void Session::txDone(bool ok)
    {
        Out& o = *_cur;
        _cur = nullptr;
        const uint8_t type = o.type, flags = o.flags;
        const uint16_t chan = o.chan, n = o.n, at = o.at;
        o.n = 0;
        if (&o != &_data) { _ctlHead = (uint8_t)((_ctlHead + 1) % CTL_SLOTS); --_ctlN; }
        if (ok) {
            ++_st.txFrames;
            _st.txBytes += n;
            if (type == EVENT) ++_st.events;
            // The request's last frame is out: now the app's answer.
            if ((type == NET_REQ || type == NET_BODY) && (flags & FIN) && chan == _netId && _netSt == NetSt::Send)
                _netSt = NetSt::Head;
            return;
        }
        if (at) ++_st.cut; else ++_st.dropped;
        switch (type) {
            // Cut or not sent: the app hears 91 (if the link is back), never silence.
            case HTTP_RESP: if (!(flags & FIN)) fail(chan, "send"); break;
            case NET_REQ:
            case NET_BODY:  if (chan == _netId && _netSt == NetSt::Send) netFail("send"); break;
            case NET_ACK:   _netAcked = 0; break;   // the credit said again next pass
            case HELLO_ACK:
                ++_st.helloAckFails;
                LOGW("link", "%s: HELLO_ACK not sent (%s, wait %lu ms)", tag(), at ? "cut" : "dropped",
                     (unsigned long)_st.txWaitMax);
                break;
            default: break;
        }
    }

    // The next data frame: the channels' answers, the alerts and the net
    // request take turns, so none waits out a long one of another.
    bool Session::nextData(uint32_t now)
    {
        for (uint8_t k = 0; k < 3; ++k) {
            const uint8_t p = (uint8_t)((_rr + k) % 3);
            if (p == 0 ? pumpOne(now) : p == 1 ? eventOne(now) : netOne()) {
                _rr = (uint8_t)((p + 1) % 3);
                return true;
            }
        }
        return false;
    }

    // Control first, at frame boundaries; then data. As much as the pipe
    // takes now, at most TX_FRAMES_PASS frames.
    void Session::txService()
    {
        const uint32_t now = millis();
        for (uint8_t k = 0; k < TX_FRAMES_PASS; ++k) {
            if (!_cur) {
                if (_ctlN) _cur = &_ctl[_ctlHead];
                else if (_data.n || nextData(now)) _cur = &_data;
                else return;
                _txSince = millis();
                _txRx = _st.rxFrames;
            }
            Out& o = *_cur;
            if (o.at == 0 && !_pipe.ready()) {
                if (txStalled(millis())) continue;
                return;
            }
            const size_t w = _pipe.write(o.buf + o.at, o.n - o.at);
            if (w) {
                o.at = (uint16_t)(o.at + w);
                _txSince = millis();
                _txRx = _st.rxFrames;
            }
            if (o.at >= o.n) { txDone(true); continue; }
            if (w) return;                       // the pipe is full: next pass
            if (!txStalled(millis())) return;
        }
    }

    // ---- channels -------------------------------------------------------------
    void Session::closeChan(uint16_t c)
    {
        if (c >= CHANS || _ch[c].fd < 0) return;
        ::close(_ch[c].fd);
        _ch[c].fd = -1;
        _ch[c].connecting = false;
        _ch[c].reqN = _ch[c].reqAt = 0;
    }

    void Session::fail(uint16_t c, const char* why)
    {
        closeChan(c);
        ++_st.fails;
        controlText(HTTP_FAIL, c, why);
    }

    void Session::closeAll()
    {
        for (uint16_t c = 0; c < CHANS; ++c) closeChan(c);
    }

    // Non-blocking: connect() goes on in sockService.
    bool Session::openChan(uint16_t c)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) return false;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(80);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const int cr = ::connect(fd, (struct sockaddr*)&a, sizeof(a));
        if (cr != 0 && errno != EINPROGRESS) {
            ::close(fd);
            return false;
        }
        Chan& ch = _ch[c];
        ch.fd = fd;
        ch.connecting = cr != 0;
        ch.since = ch.lastMs = millis();
        ch.reqN = ch.reqAt = 0;
        ++_st.reqs;
        return true;
    }

    bool Session::reqWaiting() const
    {
        for (const auto& c : _ch) if (c.fd >= 0 && c.reqAt < c.reqN) return true;
        return false;
    }

    // The channel's request bytes to the server, as many as it takes now.
    void Session::reqFlush(uint16_t c, uint32_t now)
    {
        Chan& ch = _ch[c];
        while (ch.fd >= 0 && !ch.connecting && ch.reqAt < ch.reqN) {
            const int w = ::send(ch.fd, ch.req + ch.reqAt, ch.reqN - ch.reqAt, MSG_DONTWAIT);
            if (w > 0) {
                ch.reqAt = (uint16_t)(ch.reqAt + w);
                if (now - ch.since > _st.sockSendMax) _st.sockSendMax = now - ch.since;
                ch.since = now;
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (now - ch.since > SOCK_GIVE_UP_MS) fail(c, "send");
                return;
            }
            fail(c, "send");
            return;
        }
        if (ch.fd >= 0 && ch.reqAt >= ch.reqN) ch.reqN = ch.reqAt = 0;
    }

    // connect() done yet, then the request bytes.
    void Session::sockService(uint32_t now)
    {
        for (uint16_t c = 0; c < CHANS; ++c) {
            Chan& ch = _ch[c];
            if (ch.fd < 0) continue;
            if (ch.connecting) {
                fd_set w;
                FD_ZERO(&w);
                FD_SET(ch.fd, &w);
                struct timeval tv = {0, 0};
                if (::select(ch.fd + 1, nullptr, &w, nullptr, &tv) <= 0) {
                    if (now - ch.since > SOCK_GIVE_UP_MS) fail(c, "connect");
                    continue;
                }
                int err = 0;
                socklen_t l = sizeof(err);
                getsockopt(ch.fd, SOL_SOCKET, SO_ERROR, &err, &l);
                if (now - ch.since > _st.connectMax) _st.connectMax = now - ch.since;
                if (err) { fail(c, "connect"); continue; }
                ch.connecting = false;
                ch.since = now;
            }
            reqFlush(c, now);
        }
    }

    void Session::onRequest(const Frame& f)
    {
        const uint16_t c = f.chan;
        if (c >= CHANS) { controlText(HTTP_FAIL, c, "busy"); ++_st.fails; return; }
        if (_ch[c].fd < 0 && !openChan(c)) { fail(c, "connect"); return; }
        Chan& ch = _ch[c];
        if (ch.reqAt) {
            memmove(ch.req, ch.req + ch.reqAt, ch.reqN - ch.reqAt);
            ch.reqN = (uint16_t)(ch.reqN - ch.reqAt);
            ch.reqAt = 0;
        }
        // Never full: the pipe is not read while bytes wait (Chan).
        if (ch.reqN + f.len > sizeof(ch.req)) { fail(c, "send"); return; }
        const uint32_t now = millis();
        if (!ch.reqN && !ch.connecting) ch.since = now;
        memcpy(ch.req + ch.reqN, f.payload, f.len);
        ch.reqN = (uint16_t)(ch.reqN + f.len);
        ch.lastMs = now;
        reqFlush(c, now);
    }

    // Server to app: one recv into the data frame; the server closing is FIN.
    bool Session::pumpOne(uint32_t now)
    {
        for (uint16_t k = 0; k < CHANS; ++k) {
            const uint16_t c = (uint16_t)((_pumpC + k) % CHANS);
            Chan& ch = _ch[c];
            if (ch.fd < 0 || ch.connecting) continue;
            const int r = ::recv(ch.fd, _body, sizeof(_body), MSG_DONTWAIT);
            if (r >= 0) {
                if (r) ch.lastMs = now;
                else closeChan(c);
                fill(_data, HTTP_RESP, r ? 0 : FIN, c, _body, (size_t)r);
                _pumpC = (uint8_t)((c + 1) % CHANS);
                return true;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) fail(c, "recv");
            else if (ch.reqAt >= ch.reqN && (int32_t)(now - ch.lastMs) > (int32_t)IDLE_MS)   // signed: a request may have opened it after `now`
                fail(c, "timeout");
        }
        return false;
    }

    // ---- the app's internet -----------------------------------------------
    void Session::netFail(const char* why)
    {
        snprintf(_netWhy, sizeof(_netWhy), "%s", why);
        _netSt = NetSt::Fail;
    }

    // applink, every pass: the credit back, a cancel (control frames).
    void Session::netService()
    {
        if (_netCancel && control(NET_CANCEL, FIN, _netCancelId, nullptr, 0)) _netCancel = false;
        if (_netSt == NetSt::Body) {
            const uint32_t taken = _netTaken;
            if (taken - _netAcked >= NET_ACK_STEP) {
                const uint8_t p[4] = {(uint8_t)taken, (uint8_t)(taken >> 8), (uint8_t)(taken >> 16), (uint8_t)(taken >> 24)};
                if (control(NET_ACK, FIN, _netId, p, 4)) _netAcked = taken;
            }
        }
    }

    // The request, one data frame at a time: NET_REQ, then the body.
    bool Session::netOne()
    {
        if (_netSt != NetSt::Send) return false;
        if (!_netReqOut) {
            _netReqOut = true;
            return fill(_data, NET_REQ, _netBodyLen ? 0 : FIN, _netId, (const uint8_t*)_netReq, _netReqLen);
        }
        if (_netAt >= _netBodyLen) return false;
        const size_t n = _netBodyLen - _netAt < MAX_PAYLOAD ? _netBodyLen - _netAt : MAX_PAYLOAD;
        const bool ok = fill(_data, NET_BODY, _netAt + n >= _netBodyLen ? FIN : 0, _netId, _netBody + _netAt, n);
        _netAt += n;
        return ok;
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
        _evMore = false;
        closeAll();     // a new HELLO is a new app: its old requests are gone
        // ... and the old app's data frame, unless half of it is out already.
        if (_data.n && !(_cur == &_data && _data.at)) {
            if (_cur == &_data) _cur = nullptr;
            _data.n = 0;
        }
        if (!_hello) LOGI("link", "%s: the app said HELLO (net %s)", tag(), _appNet ? "offered" : "no");
        _hello = true;
        ++_st.hellos;
        // The car (boot 291, 2026-11-22): HELLO in every 5 s, the app never
        // saw the answer. Whether it went out is the first question; a
        // give-up later is said by txDone.
        if (!control(HELLO_ACK, FIN, 0, (const uint8_t*)out, n)) {
            ++_st.helloAckFails;
            LOGW("link", "%s: HELLO_ACK not sent (%s)", tag(), _up ? "queue full" : "no host");
        }
    }

    void Session::onFrame(const Frame& f)
    {
        ++_st.rxFrames;
        _lastRx = millis();
        _anyRx = true;
        switch (f.type) {
            case HELLO:      hello(f); break;
            case PING:       control(PONG, FIN, f.chan, f.payload, f.len); break;
            case HTTP_REQ:   onRequest(f); break;
            case HTTP_CLOSE: closeChan(f.chan); break;
            case NET_HEAD:
            case NET_DATA:
            case NET_FAIL:   onNet(f); break;
            default: break;        // an app newer than us: ignored
        }
    }

    // One alert into the data frame. An app gone quiet: the alerts wait
    // (_evSeq stays) for it or the next HELLO.
    bool Session::eventOne(uint32_t now)
    {
        if (!_hello || !hostLive()) return false;
        if (!_evMore && now - _evAt < EVENT_MS) return false;
        _evAt = now;
        App::Car::AlertEntry e;
        uint32_t last = 0;
        const uint8_t n = App::Car::alertsSince(_evSeq, &e, 1, last);
        if (last < _evSeq) _evSeq = 0;   // cannot happen without a reboot; be safe
        _evMore = n != 0;
        if (!n) return false;
        _evSeq = e.seq;
        JsonDocument d;
        d["t"] = "alert";
        d["seq"] = e.seq;
        d["sig"] = e.rule;
        d["value"] = e.value;
        d["text"] = e.text;
        char out[192];
        const size_t len = serializeJson(d, out, sizeof(out));
        return fill(_data, EVENT, FIN, 0, (const uint8_t*)out, len);
    }

    void Session::down()
    {
        closeAll();
        _parser.reset();
        _cur = nullptr;
        _data.n = 0;
        _ctlN = 0;
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
            sockService(now);
            // Not while a request waits for the server: the pipe holds the
            // rest (USB NAKs the host; BLE's buffer fills, `overflow` counts).
            size_t r;
            while (!reqWaiting() && (r = _pipe.read(_rx, sizeof(_rx))) > 0) {
                _st.rxBytes += r;
                _parser.feed(_rx, r, frameCb, this);
            }
            netService();
            txService();
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
        _netAt = 0;
        _netReqOut = false;
        _netSt = NetSt::Send;
        if (wake) wake();
        const uint32_t t0 = millis();
        while (_netSt == NetSt::Send || _netSt == NetSt::Head) {
            // Never back out while the body is being sent: it is the caller's.
            if (_netSt == NetSt::Head && millis() - t0 > timeoutMs) {
                _netCancelId = _netId;
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
        if (_netSt == NetSt::Body && !(_netEof && xStreamBufferIsEmpty(_netSb))) {
            _netCancelId = _netId;
            _netCancel = true;
        }
        _netSt = NetSt::Idle;
        if (wake) wake();
    }
}
#endif
