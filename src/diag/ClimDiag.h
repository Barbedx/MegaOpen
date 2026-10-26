#pragma once
#include <stddef.h>
#include <stdint.h>

// One diagnostic read on CAN A: the climate ECU's `21 C1` (DDT2000
// Climatisation_regulee_Redesign_P0_X84_SERIE, "Informations Systeme n°1"),
// which carries the cabin temperature no broadcast frame has. KWP2000 over
// ISO-TP, physical addressing, 0x744 -> 0x764.
//
// THE ONLY TRANSMIT ON THE CAR BUS. CAN A is listen-only (CLAUDE.md) except
// for what this file lets out, all of it to 0x744:
//   * the read `21 C1` (allowed()), and for the owner's one-shot sweep
//     (App::CarDiag::sweep()) the other reads DDT lists for this ECU:
//     `21 80`, `21 C0`, `21 C2`, `21 90` (trame déportée, Clim_X84_SERIE),
//     `21 A0` (trame étude);
//   * `10 C0` (open the extended session, as PyRen and DDT4all do first,
//     "ECU uses SDS 10C0"), `3E 01` (tester present), `10 81` (back to the
//     default session): the sweep, and the polling read when diag.session
//     is on (off by default);
//   * flow control `30 00 00`, only in answer to a first frame from 0x764
//     while a request waits.
// A session is opened only when the owner asks (the sweep's button, or the
// diag.session switch): opening one is what the owner saw reboot the cluster
// once, and the sweep checks that by hand, the bus watched around each step. No functional (0x7DF)
// address, no other ECU, no write, no clear. Frames are sent as PyRen's ELM
// sends them with CAF0: the bytes given, no padding.
//
// Pure (no Arduino): the host tests drive it with a fake send and a fake clock.
namespace Diag
{
    constexpr uint32_t CLIM_REQ  = 0x744;
    constexpr uint32_t CLIM_RESP = 0x764;
    constexpr uint32_t REPLY_TIMEOUT_MS = 1000;   // the first frame, and between frames
    constexpr uint32_t PENDING_TIMEOUT_MS = 5000; // after `7F 21 78`, response pending
    constexpr uint8_t  REPLY_MAX = 64;

    // The allowlist, the one choke point: the request bytes (no ISO-TP PCI)
    // this file may send: the reads `21 80 / C0 / C1 / C2 / 90 / A0`, and
    // `10 C0`, `3E 01`, `10 81`. Nothing else: no write (3B), no clear (14),
    // no routine (31), no reset (11), no IO control (30).
    bool allowed(const uint8_t* req, uint8_t n);

    // false: not sent (the controller refused or is listen-only).
    using SendFn = bool (*)(uint32_t id, const uint8_t* data, uint8_t len, void* ctx);

    class Exchange {
    public:
        enum class Status : uint8_t {
            Idle,        // nothing asked
            Waiting,     // the request is out, the reply not complete
            Ok,          // a positive reply (SID + 0x40) is in reply()
            Negative,    // `7F SID NRC`: nrc()
            Timeout,     // no frame for REPLY_TIMEOUT_MS
            Refused,     // not on the allowlist: nothing was sent
            SendFailed,  // the controller did not take the frame
            Bad,         // a reply that is not ISO-TP sense (sequence, length)
        };

        Exchange(SendFn send, void* ctx) : _send(send), _ctx(ctx) {}

        // One request at a time; false (and nothing sent) while one waits.
        bool start(const uint8_t* req, uint8_t n, uint32_t nowMs);
        // Every frame of the car bus may come here; only CLIM_RESP is read.
        void onFrame(uint32_t id, const uint8_t* data, uint8_t len, uint32_t nowMs);
        void tick(uint32_t nowMs);
        // Back to Idle after a result was read.
        void clear() { _st = Status::Idle; }

        Status         status() const { return _st; }
        uint8_t        nrc() const { return _nrc; }
        const uint8_t* reply() const { return _buf; }
        uint8_t        replyLen() const { return _have; }
        static const char* statusId(Status s);

    private:
        void done(Status s) { _st = s; }

        SendFn   _send;
        void*    _ctx;
        Status   _st = Status::Idle;
        uint8_t  _sid = 0;
        uint8_t  _nrc = 0;
        uint8_t  _buf[REPLY_MAX] = {};
        uint8_t  _want = 0, _have = 0, _seq = 0;
        uint32_t _lastMs = 0;
        uint32_t _limitMs = REPLY_TIMEOUT_MS;
    };

    // `61 C1 ...` as DDT numbers it (byte 1 = 0x61): 4 outside -40 °C,
    // 6 coolant -40 °C, 8 evaporator x0.5 -40 °C, 9 cabin x0.5 -40 °C,
    // 10 sun x3 W/m², 12 battery x0.078 V. false: too short or not 61 C1.
    struct ClimValues {
        int16_t  cabinX10 = 0;      // °C x10
        int16_t  evapX10 = 0;       // °C x10
        int16_t  outsideC = 0;
        int16_t  coolantC = 0;
        uint16_t sunWm2 = 0;
        uint16_t battX100 = 0;      // V x100; 0 when the reply stops before byte 12
    };
    bool decode21C1(const uint8_t* r, uint8_t n, ClimValues& out);

    // `61 90 ...`, "trame déportée LDB vers LDA" in Clim_X84_SERIE (other
    // software versions than ours; a hypothesis until it answers): byte 3 bits
    // 7-5 recirculation (0 auto, 1 recycle, 2 outside), bits 4-0 the driver's
    // setpoint (x+30)/2 °C; byte 4 high nibble distribution (0 auto, 1 vent,
    // 2 feet/vent, 3 tri, 4 feet/demist, 5 demist), low nibble blower (0 auto,
    // 1 stop, 2..9 = 1..8); byte 5 low nibble A/C request (0 auto, 1 off, 2 on).
    struct Clim2190 {
        uint8_t  recirc = 0, dist = 0, blower = 0, ac = 0;
        uint16_t setX10 = 0;        // °C x10
    };
    bool decode2190(const uint8_t* r, uint8_t n, Clim2190& out);
}
