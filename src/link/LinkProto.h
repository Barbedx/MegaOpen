#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../bus/Frame.h"

// UART protocol between board V (C3 on the car's OBD2 CAN) and the brain
// (DevKit). Portable (no Arduino) so both ends and host tests share one codec.
//
// Carries raw CAN frames, not decoded signals: the Megane decode lives on the
// brain (sniffer), board V knows nothing about the car.
//
// Binary: 1000-2000 frames/s is ~26 kB/s as 13-byte records vs ~90 kB/s at
// 921600 baud; text would not fit.
//
//   0x5A  type  len  payload[len]  crc8       len <= 32, crc over type..payload
//
// Decoder resyncs on 0x5A, so a lost byte costs one record. No ACK, no sequence.
namespace Link
{
    inline constexpr uint8_t SYNC        = 0x5A;
    inline constexpr uint8_t MAX_PAYLOAD = 32;
    inline constexpr uint8_t MAX_RECORD  = 4 + MAX_PAYLOAD;
    inline constexpr uint32_t BAUD       = 921600;

    enum Type : uint8_t {
        T_CAN    = 0x01,   // V -> brain: one CAN frame. id(4, bit31 = extended) dlc(1) data(dlc)
        T_STATUS = 0x02,   // V -> brain, once a second: see Status
        T_HELLO  = 0x03,   // either way, on boot: version(1)
        T_FILTER = 0x10,   // brain -> V: mode(1) n(1) id(4)*n — see FilterMode
        // brain -> V: fps(2). Non-zero makes V synthesise frames at that rate
        // through the real filter/encoder path (bench test); 0 stops. V reports
        // the rate in Status.sim so fake traffic is never mistaken for a car.
        T_SIM    = 0x11,
    };

    // What board V forwards; set by the brain, V boots as F_ALL.
    enum FilterMode : uint8_t {
        F_ALL          = 0,   // every frame
        F_CHANGES      = 1,   // a frame only when its payload differs from the last one
                              // with that id, plus one per id per second regardless
        F_LIST         = 2,   // only the listed ids, every frame
        F_LIST_CHANGES = 3,   // only the listed ids, and only when they change
    };
    inline constexpr uint8_t MAX_FILTER_IDS = 7;     // (32 - 2) / 4

    struct Status {
        uint32_t frames;        // received from the bus since boot
        uint32_t forwarded;     // sent up the link since boot
        uint16_t fps;           // bus frames in the last second
        uint8_t  rxErr;         // TWAI receive error counter
        uint8_t  mode;          // FilterMode in force
        uint32_t busErr;        // bus error alerts since boot
        uint8_t  state;         // 0 stopped, 1 running, 2 bus-off, 3 recovering
        uint16_t sim;           // simulated frames per second, 0 = a real bus only
    };

    uint8_t crc8(const uint8_t* p, size_t n);

    // ---- encoding: each returns the record length written to `out` (>= MAX_RECORD) ----
    uint8_t encodeCan(const Frame& f, uint8_t* out);
    uint8_t encodeStatus(const Status& s, uint8_t* out);
    uint8_t encodeHello(uint8_t version, uint8_t* out);
    uint8_t encodeFilter(FilterMode mode, const uint32_t* ids, uint8_t n, uint8_t* out);
    uint8_t encodeSim(uint16_t fps, uint8_t* out);

    // ---- decoding: feed bytes, get records ------------------------------------
    struct Record {
        Type    type;
        uint8_t len;
        uint8_t payload[MAX_PAYLOAD];

        bool asCan(Frame& f) const;
        bool asStatus(Status& s) const;
        bool asFilter(FilterMode& mode, uint32_t* ids, uint8_t& n) const;   // ids[MAX_FILTER_IDS]
        bool asSim(uint16_t& fps) const;
    };

    class Decoder {
    public:
        // Returns true when `out` holds a complete, CRC-valid record.
        bool feed(uint8_t b, Record& out);
        uint32_t records() const { return _records; }
        uint32_t crcErrors() const { return _crcErrors; }
        uint32_t resyncs() const { return _resyncs; }

    private:
        enum class St : uint8_t { Sync, Type, Len, Payload, Crc };
        St      _st = St::Sync;
        Record  _cur{};
        uint8_t _have = 0;
        uint32_t _records = 0, _crcErrors = 0, _resyncs = 0;
    };

    // ---- the change filter, for board V ------------------------------------------
    // "On change, plus once a second" per id (F_CHANGES). Portable for host tests.
    class ChangeFilter {
    public:
        static constexpr uint8_t MAX_IDS = 96;
        static constexpr uint32_t REFRESH_MS = 1000;

        void setMode(FilterMode m) { _mode = m; }
        FilterMode mode() const { return _mode; }
        void setList(const uint32_t* ids, uint8_t n);

        bool pass(const Frame& f, uint32_t nowMs);
        void reset();

        uint8_t  tracked() const { return _n; }
        uint32_t overflow() const { return _overflow; }

    private:
        struct Slot { uint32_t id; uint8_t len; uint8_t data[8]; uint32_t sentMs; bool used; };
        Slot     _s[MAX_IDS] = {};
        uint8_t  _n = 0;
        uint32_t _overflow = 0;
        FilterMode _mode = F_ALL;
        uint32_t _list[MAX_FILTER_IDS] = {};
        uint8_t  _listN = 0;

        bool listed(uint32_t id) const;
    };
}
