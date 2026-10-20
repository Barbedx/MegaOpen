#pragma once
#include "../Features.h"
#if FEATURE_BLE
#include "../keys/IKeySink.h"

// The phone (AMS) and the head unit's BLE keyboard (HID) as key sinks that
// never touch the radio (docs/design/event-tasks.md, Step 2).
//
// An AMS command is a GATT write WITH response: 30-130 ms each, and a volume
// press is fifteen of them, up to 1.9 s. Run inline, that was 1.9 s of every
// other sink — the UART included — waiting behind it. So on the keys task a BLE
// sink only filters by class and posts the key to the BLE command queue;
// ble_svc performs it (BleKeyWorker, BleKeySinks.h), on the same task as
// ServiceAms, which also closes the race between the two over one client.
//
// HID rides the same queue to ble_svc and is handled there before any AMS
// write (owner decision Q2: BleHub, the MeganeCAN-derived hub, is not changed
// to let the keys task drive HID). UART is the main sink; HID is not in use.
namespace Sinks
{
    // One key for ble_svc. Plain bytes: copied through a FreeRTOS queue.
    struct BleCmd {
        enum class Target : uint8_t { Ams = 0, Hid = 1 };
        Target       target = Target::Ams;
        Keys::Action action = Keys::Action::None;
        uint32_t     atMs   = 0;     // when the keys task posted it, for the 300 ms rule
    };

    namespace BleKeyQueue
    {
        constexpr uint8_t DEPTH = 8;
        // Created in setup() before any key can be posted, whether or not BLE
        // starts: a proxy with nobody behind it drops and counts.
        bool begin();
        // The keys task. Zero wait.
        bool post(const BleCmd& c);
        // ble_svc: the first item, waiting up to waitMs for it.
        bool take(BleCmd& out, uint32_t waitMs);
        uint32_t dropped();
    }

    class BleKeyProxy : public Keys::ISink {
    public:
        explicit BleKeyProxy(BleCmd::Target t) : _target(t) {}

        // Routing mask, applied by the keys task from the staged routing.
        void setClasses(uint8_t m) { _mask = m; }
        uint8_t classes() const { return _mask; }

        void onKey(const Keys::Event& e) override;
        const char* id() const override { return _target == BleCmd::Target::Ams ? "ams" : "hid"; }

        uint32_t posted() const { return _posted; }

    private:
        BleCmd::Target _target;
        volatile uint8_t _mask = 0;
        uint32_t _posted = 0;
    };
}
#endif
