// Whole-body gate: with FEATURE_BLE=0 NimBLE's static constructors would
// survive --gc-sections.
#include "../Features.h"
#if FEATURE_BLE

#include "HidRole.h"
#include "BleHub.h"
#include "BlePeerTable.h"
#include "../utils/Log.h"

#include <NimBLEHIDDevice.h>

namespace Hid
{
    namespace
    {
        NimBLEHIDDevice      *gHid       = nullptr;
        NimBLECharacteristic *gInput     = nullptr;
        bool                  gBuilt     = false;

        // Deferred release: press() queues, service() sends the zero report.
        bool     gReleasePending = false;
        uint32_t gPressedMs      = 0;
        const uint32_t KEY_HOLD_MS = 30; // long enough to span one connection event

        // Consumer Control descriptor, one 16-bit usage per report. Known-good; do not edit.
        const uint8_t kReportMap[] = {
            0x05, 0x0C,       // Usage Page (Consumer)
            0x09, 0x01,       // Usage (Consumer Control)
            0xA1, 0x01,       // Collection (Application)
            0x85, 0x01,       //   Report ID (1)
            0x15, 0x00,       //   Logical Minimum (0)
            0x26, 0xFF, 0x03, //   Logical Maximum (1023)
            0x75, 0x10,       //   Report Size (16)
            0x95, 0x01,       //   Report Count (1)
            0x19, 0x00,       //   Usage Minimum (0)
            0x2A, 0xFF, 0x03, //   Usage Maximum (1023)
            0x81, 0x00,       //   Input (Data, Array, Absolute)
            0xC0              // End Collection
        };

        // Subscribing to the input report identifies the head unit (an iPhone never
        // does); this also stops AMS probing that peer.
        class ReportCallbacks : public NimBLECharacteristicCallbacks
        {
            void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &connInfo, uint16_t subValue) override
            {
                const uint16_t h = connInfo.getConnHandle();
                if (subValue == 0)
                {
                    LOGI("HID", "unsubscribed h=%u", (unsigned)h);
                    return;
                }
                LOGI("HID", "input report subscribed by h=%u — classifying as head unit", (unsigned)h);
                BleHub::classifyAsHid(h, connInfo.getIdAddress().toString().c_str());
            }
        };

        ReportCallbacks gReportCb;
    } // namespace

    void begin(NimBLEServer *server)
    {
        if (gBuilt || !server)
            return;

        gHid = new NimBLEHIDDevice(server);
        gHid->setManufacturer("gycer");
        gHid->setPnp(0x02, 0x05AC, 0x0239, 0x0110);
        gHid->setHidInfo(0x00, 0x01);
        gHid->setReportMap((uint8_t *)kReportMap, sizeof(kReportMap));

        gInput = gHid->getInputReport(1);
        if (gInput)
            gInput->setCallbacks(&gReportCb);

        // No startServices(): BleHub calls NimBLEServer::start() after all roles.
        gBuilt = true;
        LOGI("HID", "service tree built");
    }

    bool built() { return gBuilt; }

    uint16_t connHandle()
    {
        Ble::Peer *p = BleHub::peers().byRole(Ble::PeerRole::Hid);
        return p ? p->handle : BLE_HS_CONN_HANDLE_NONE;
    }

    bool connected()
    {
        return gBuilt && gInput && connHandle() != BLE_HS_CONN_HANDLE_NONE;
    }

    bool press(uint16_t usageCode)
    {
        if (!connected())
        {
            LOGW("HID", "press 0x%04X dropped — head unit not connected", usageCode);
            return false;
        }
        // A press still in flight: send its release first so codes don't merge.
        if (gReleasePending)
        {
            const uint8_t rel[2] = {0, 0};
            gInput->setValue(rel, sizeof(rel));
            gInput->notify(connHandle());
            gReleasePending = false;
        }

        const uint8_t down[2] = {(uint8_t)(usageCode & 0xFF), (uint8_t)(usageCode >> 8)};
        gInput->setValue(down, sizeof(down));
        gInput->notify(connHandle());

        gReleasePending = true;
        gPressedMs      = millis();
        return true;
    }

    void service()
    {
        if (!gReleasePending)
            return;
        if (millis() - gPressedMs < KEY_HOLD_MS)
            return;

        if (!connected())
        {
            // Link down: the head unit dropped the key state with it.
            gReleasePending = false;
            return;
        }
        gReleasePending = false;
        const uint8_t rel[2] = {0, 0};
        gInput->setValue(rel, sizeof(rel));
        gInput->notify(connHandle());
    }
} // namespace Hid

#endif  // FEATURE_BLE
