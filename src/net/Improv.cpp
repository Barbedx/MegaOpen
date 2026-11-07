#include "Improv.h"
#include <string.h>
#include <stdio.h>

namespace Improv {

static const uint8_t HDR[6] = {'I', 'M', 'P', 'R', 'O', 'V'};
static constexpr uint8_t VERSION = 1;

size_t frame(uint8_t type, const uint8_t* data, size_t len, uint8_t* out, size_t cap)
{
    const size_t n = 6 + 3 + len + 1 + 1;
    if (len > MAX_DATA || cap < n) return 0;
    memcpy(out, HDR, 6);
    out[6] = VERSION;
    out[7] = type;
    out[8] = (uint8_t)len;
    if (len) memcpy(out + 9, data, len);
    uint8_t sum = 0;
    for (size_t i = 0; i < 9 + len; ++i) sum += out[i];
    out[9 + len] = sum;
    out[10 + len] = '\n';
    return n;
}

void Session::feed(uint8_t c, uint32_t nowMs)
{
    // The header, byte by byte: anything else (a stray log byte, a terminal's
    // keystroke) starts over, and a byte that breaks the header may itself
    // begin a new one.
    if (_n < 6) {
        if (c == HDR[_n]) { _rx[_n++] = c; return; }
        _n = (c == HDR[0]) ? 1 : 0;
        if (_n) _rx[0] = c;
        return;
    }
    _rx[_n++] = c;
    if (_n < 9) return;
    const size_t len = _rx[8];
    if (_n < 9 + len + 1) return;
    // A whole packet: header, version, type, length, data, checksum.
    uint8_t sum = 0;
    for (size_t i = 0; i < 9 + len; ++i) sum += _rx[i];
    const bool ok = sum == _rx[9 + len] && _rx[6] == VERSION;
    const uint8_t type = _rx[7];
    _n = 0;
    if (!ok) { ++_bad; sendError(InvalidRpc); return; }
    ++_packets;
    onPacket(type, _rx + 9, (uint8_t)len, nowMs);
}

void Session::onPacket(uint8_t type, const uint8_t* d, uint8_t len, uint32_t nowMs)
{
    if (type == Rpc) onRpc(d, len, nowMs);
    // The client sends nothing else; other types are ignored.
}

// RPC data: command | length of the rest | the rest. Strings inside are
// length-prefixed.
void Session::onRpc(const uint8_t* d, uint8_t len, uint32_t nowMs)
{
    if (len < 2 || d[1] != len - 2) { ++_bad; sendError(InvalidRpc); return; }
    const uint8_t cmd = d[0];
    const uint8_t* a = d + 2;
    const uint8_t an = d[1];

    switch (cmd) {
    case WifiSettings: {
        // ssid_len ssid pass_len pass
        if (an < 1 || a[0] > 32 || 1 + a[0] + 1 > an) { sendError(InvalidRpc); return; }
        const uint8_t sl = a[0];
        const uint8_t pl = a[1 + sl];
        if (pl > 64 || 1 + sl + 1 + pl != an || sl == 0) { sendError(InvalidRpc); return; }
        char ssid[33], pass[65];
        memcpy(ssid, a + 1, sl);          ssid[sl] = 0;
        memcpy(pass, a + 2 + sl, pl);     pass[pl] = 0;
        sendError(NoError);
        _b.provision(ssid, pass);
        _joining = true;
        _joinAt = nowMs;
        _state = Provisioning;
        sendState(Provisioning);
        return;
    }
    case GetState: {
        if (_joining) { sendState(Provisioning); return; }
        if (_b.connected()) {
            _state = Provisioned;
            sendState(Provisioned);
            char u[64];
            _b.url(u, sizeof(u));
            sendResult(GetState, u);
        } else {
            _state = Authorized;
            sendState(Authorized);
        }
        return;
    }
    case GetInfo:
        sendResult(GetInfo, _b.firmware(), _b.version(), _b.chip(), _b.deviceName());
        return;
    case GetNetworks:
        if (!_scanning) _scanning = _b.scanStart();
        if (!_scanning) sendResult(GetNetworks);   // nothing to list: the empty end
        return;
    default:
        sendError(UnknownRpc);
        return;
    }
}

void Session::tick(uint32_t nowMs)
{
    if (_scanning) {
        const int n = _b.scanDone(_nets, MAX_NETS);
        if (n >= 0) {
            _scanning = false;
            for (int i = 0; i < n; ++i) {
                char rssi[8];
                snprintf(rssi, sizeof(rssi), "%d", _nets[i].rssi);
                sendResult(GetNetworks, _nets[i].ssid, rssi, _nets[i].secure ? "YES" : "NO");
            }
            sendResult(GetNetworks);   // an empty result ends the list
        }
    }
    if (_joining) {
        const Board::Join j = _b.join();
        if (j == Board::Join::Joined) {
            _joining = false;
            _state = Provisioned;
            sendState(Provisioned);
            char u[64];
            _b.url(u, sizeof(u));
            sendResult(WifiSettings, u);
        } else if (j == Board::Join::Failed || (uint32_t)(nowMs - _joinAt) >= JOIN_TIMEOUT_MS) {
            _joining = false;
            _state = Authorized;
            sendError(UnableToConnect);
            sendState(Authorized);
        }
    }
}

void Session::sendState(State s)
{
    const uint8_t b = s;
    send(CurrentState, &b, 1);
}

void Session::sendError(Error e)
{
    const uint8_t b = e;
    send(ErrorState, &b, 1);
}

void Session::sendResult(uint8_t cmd, const char* s0, const char* s1, const char* s2, const char* s3)
{
    uint8_t d[MAX_DATA];
    size_t n = 2;
    const char* s[4] = {s0, s1, s2, s3};
    for (const char* x : s) {
        if (!x) break;
        size_t l = strlen(x);
        if (l > 64) l = 64;
        if (n + 1 + l > MAX_DATA) break;
        d[n++] = (uint8_t)l;
        memcpy(d + n, x, l);
        n += l;
    }
    d[0] = cmd;
    d[1] = (uint8_t)(n - 2);
    send(RpcResult, d, n);
}

void Session::send(uint8_t type, const uint8_t* d, size_t len)
{
    uint8_t out[9 + MAX_DATA + 2];
    const size_t n = frame(type, d, len, out, sizeof(out));
    if (n) _p.write(out, n);
}

}  // namespace Improv
