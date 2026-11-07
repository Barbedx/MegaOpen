#pragma once
#include <stddef.h>
#include <stdint.h>

// Improv Wi-Fi over serial (https://www.improv-wifi.com/serial/), version 1:
// the web installer (installer/, ESP Web Tools) asks the board on its USB
// serial what it is and which networks it sees, and hands it the WiFi
// password, so a freshly flashed board joins the home network without a trip
// to its access point.
//
// Portable: no Arduino here. The host tests drive it through the two
// interfaces; src/net/ImprovSerial.cpp is the board's side (Serial, the
// WifiManager, WiFi scans) and runs it on net_svc beside WifiManager::loop().
//
// A packet:  "IMPROV" | version 1 | type | length | data[length] | checksum
// checksum = the low byte of the sum of every byte before it. The board ends
// each packet it sends with '\n' (as ESPHome does), so a terminal reading the
// log sees one line per packet. Bytes that are not a packet are skipped: the
// log shares the port and the client parses around it, and so does this side.
namespace Improv {

enum Type : uint8_t { CurrentState = 0x01, ErrorState = 0x02, Rpc = 0x03, RpcResult = 0x04 };
enum State : uint8_t { Authorized = 0x02, Provisioning = 0x03, Provisioned = 0x04 };
enum Error : uint8_t { NoError = 0x00, InvalidRpc = 0x01, UnknownRpc = 0x02,
                       UnableToConnect = 0x03, Unknown = 0xFF };
enum Cmd : uint8_t { WifiSettings = 0x01, GetState = 0x02, GetInfo = 0x03, GetNetworks = 0x04 };

constexpr size_t MAX_DATA = 255;
constexpr uint32_t JOIN_TIMEOUT_MS = 20000;   // WifiManager's own is 15 s
constexpr int MAX_NETS = 16;                  // listed, strongest first

// One network from a scan.
struct Net {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;
};

// What the protocol needs from the board.
class Board {
public:
    enum class Join : uint8_t { None, Pending, Joined, Failed };
    virtual ~Board() = default;
    // Station up on a network: the reply to "state" is Provisioned with url().
    virtual bool connected() = 0;
    // "http://<address>" of the station, into out (cap bytes, NUL-terminated).
    virtual void url(char* out, size_t cap) = 0;
    // Firmware name, version, chip, device name.
    virtual const char* firmware() = 0;
    virtual const char* version() = 0;
    virtual const char* chip() = 0;
    virtual const char* deviceName() = 0;
    // Save the network as the first one and join it now. join() reports.
    virtual void provision(const char* ssid, const char* pass) = 0;
    virtual Join join() = 0;
    // A scan: start, then scanDone() until it returns a count (>= 0) of the
    // nets it wrote into out (at most cap); -1 while it runs.
    virtual bool scanStart() = 0;
    virtual int  scanDone(Net* out, int cap) = 0;
};

// Where the bytes go (one call per packet, so a packet is never split by a
// log line written from another task).
class Port {
public:
    virtual ~Port() = default;
    virtual void write(const uint8_t* data, size_t len) = 0;
};

// Builds one packet (header, data, checksum, '\n') into out; returns its size,
// 0 when out is too small or len > MAX_DATA.
size_t frame(uint8_t type, const uint8_t* data, size_t len, uint8_t* out, size_t cap);

// The receive side and the session: feed() every byte that arrives, tick()
// every pass. Single task.
class Session {
public:
    Session(Board& b, Port& p) : _b(b), _p(p) {}

    void feed(uint8_t c, uint32_t nowMs);
    void tick(uint32_t nowMs);

    State state() const { return _state; }
    uint32_t packets() const { return _packets; }   // good packets received
    uint32_t bad() const { return _bad; }           // checksum or format errors

private:
    void onPacket(uint8_t type, const uint8_t* d, uint8_t len, uint32_t nowMs);
    void onRpc(const uint8_t* d, uint8_t len, uint32_t nowMs);
    void sendState(State s);
    void sendError(Error e);
    // An RPC result: cmd and up to 4 strings (nullptr ends the list early).
    void sendResult(uint8_t cmd, const char* s0 = nullptr, const char* s1 = nullptr,
                    const char* s2 = nullptr, const char* s3 = nullptr);
    void send(uint8_t type, const uint8_t* d, size_t len);

    Board& _b;
    Port&  _p;

    // Receive: bytes of the packet so far (header included).
    uint8_t  _rx[9 + MAX_DATA + 1];
    size_t   _n = 0;

    State    _state = Authorized;
    bool     _joining = false;
    uint32_t _joinAt = 0;
    bool     _scanning = false;
    Net      _nets[MAX_NETS];   // here, not on net_svc's 6 kB stack
    uint32_t _packets = 0, _bad = 0;
};

}  // namespace Improv
