#include "UsbPipe.h"
#if FEATURE_USB_LINK
#include <Arduino.h>
#include <string.h>
#include "esp32-hal-tinyusb.h"
#include "USB.h"
#include "soc/usb_dwc_struct.h"
#include "../utils/LogBus.h"

namespace UsbPipe
{
    namespace
    {
        // ---- the vendor interface ---------------------------------------
        // Registered with TinyUSB from a static constructor: the core starts
        // USB in app_main (CDC on boot), before setup(). The core's USBVendor
        // is not used: its byte queue drops what does not fit, where reading
        // TinyUSB's FIFO ourselves leaves the host NAKed until we read.
        uint16_t loadDescriptor(uint8_t* dst, uint8_t* itf)
        {
            const uint8_t str = tinyusb_add_string_descriptor("MegaOpen link");
            const uint8_t ep = tinyusb_get_free_duplex_endpoint();
            TU_VERIFY(ep != 0);
            uint8_t d[TUD_VENDOR_DESC_LEN] = {
                TUD_VENDOR_DESCRIPTOR(*itf, str, ep, (uint8_t)(0x80 | ep), 64)
            };
            *itf += 1;
            memcpy(dst, d, TUD_VENDOR_DESC_LEN);
            return TUD_VENDOR_DESC_LEN;
        }
        struct Register {
            Register() { tinyusb_enable_interface(USB_INTERFACE_VENDOR, TUD_VENDOR_DESC_LEN, loadDescriptor); }
        } s_register;

        constexpr uint8_t ITF = 0;              // the one vendor interface
        // While the host is writing to us and not reading: its frames (a new
        // app's HELLO) come first.
        constexpr uint32_t TX_RX_WAITING_MS = 500;

        void (*s_wake)() = nullptr;
        BusStats s_bus;
        bool     s_busOn = false;        // SOFs lately
        bool     s_sofSeen = false;
        uint32_t s_sof = 0;              // the last DSTS.SOFFN read
        uint32_t s_sofAt = 0;

        class Pipe final : public Link::Pipe {
        public:
            const char* tag() const override { return "usb"; }
            bool up() override { return tud_vendor_n_mounted(ITF); }
            size_t read(uint8_t* buf, size_t n) override
            {
                const uint32_t avail = tud_vendor_n_available(ITF);
                if (!avail) return 0;
                return tud_vendor_n_read(ITF, buf, avail < n ? avail : n);
            }
            // The whole frame into the FIFO, waiting while the host reads. An
            // unplug is seen at once (tud_vendor_n_mounted), so giveUpMs only
            // bounds a host that hangs.
            Tx write(const uint8_t* f, size_t n, uint32_t giveUpMs, uint32_t& waitMax) override
            {
                size_t at = 0;
                uint32_t since = millis();
                while (at < n) {
                    if (!tud_vendor_n_mounted(ITF)) return at ? Tx::Cut : Tx::Dropped;
                    const uint32_t room = tud_vendor_n_write_available(ITF);
                    if (!room) {
                        tud_vendor_n_write_flush(ITF);
                        const uint32_t w = millis() - since;
                        if (w > waitMax) waitMax = w;
                        const bool talking = tud_vendor_n_available(ITF) > 0 && w > TX_RX_WAITING_MS;
                        if (w > giveUpMs || talking) return at ? Tx::Cut : Tx::Dropped;
                        vTaskDelay(1);
                        continue;
                    }
                    const uint32_t w = tud_vendor_n_write(ITF, f + at, n - at < room ? n - at : room);
                    at += w;
                    if (w) since = millis();
                }
                tud_vendor_n_write_flush(ITF);
                return Tx::Ok;
            }
        } s_pipe;
    }

    Link::Pipe& begin(void (*wake)())
    {
        s_wake = wake;
        // The frame number as it stands: a change from here on is a host.
        s_sof = USB_DWC.dsts_reg.soffn;
        // What the host does to the bus, for a link that drops: the core's
        // event loop calls this (tud_mount/umount/suspend/resume_cb).
        USB.onEvent([](void*, esp_event_base_t, int32_t id, void*) {
            switch (id) {
                case ARDUINO_USB_STARTED_EVENT: ++s_bus.mounts;   break;
                case ARDUINO_USB_STOPPED_EVENT: ++s_bus.unmounts; LOGI("usb", "host: bus reset or unplugged"); break;
                case ARDUINO_USB_SUSPEND_EVENT: ++s_bus.suspends; LOGI("usb", "host: bus suspended"); break;
                case ARDUINO_USB_RESUME_EVENT:  ++s_bus.resumes;  LOGI("usb", "host: bus resumed"); break;
                default: break;
            }
        });
        return s_pipe;
    }

    // The bus itself, whatever TinyUSB reports. In the car (boots 196-204,
    // 2026-10-30/31) every link counter stayed at zero for days and could not
    // say whether a host was even there. DSTS.SOFFN moves once a millisecond
    // while a host drives the bus, enumeration or not: still, there is no
    // host, no data wire, or a dead port. tud_connected(): the host gave us
    // an address, so it read our descriptors; addressed and never configured
    // is a host that refused them.
    void watch(uint32_t now)
    {
        const uint32_t sof = USB_DWC.dsts_reg.soffn;
        if (sof != s_sof) { s_sof = sof; s_sofAt = now; s_sofSeen = true; }
        const bool on = s_sofSeen && now - s_sofAt < BUS_IDLE_MS;
        if (on != s_busOn) {
            s_busOn = on;
            if (on) { ++s_bus.busUps; LOGI("usb", "host: SOFs on the bus, a host drives it"); }
            else      LOGI("usb", "host: bus idle, no SOF for %lu ms", (unsigned long)(now - s_sofAt));
        }
        const bool addr = tud_connected();
        if (addr != s_bus.addressed) {
            s_bus.addressed = addr;
            LOGI("usb", addr ? "host: addressed the device (it reads our descriptors)" : "host: address gone");
        }
        const bool m = tud_vendor_n_mounted(ITF);
        if (m != s_bus.mounted) {
            s_bus.mounted = m;
            LOGI("usb", "link %s", m ? "up: a host configured the device" : "down");
        }
        s_bus.sof       = on;
        s_bus.suspended = tud_suspended();
        s_bus.sofAgoMs  = s_sofSeen ? now - s_sofAt : UINT32_MAX;
    }

    void bus(BusStats& out) { out = s_bus; }

    void wakeFromRx() { if (s_wake) s_wake(); }
}

// The device descriptor, in place of the core's (a weak default there). With
// WebUSB on, the core makes the whole device class 02 (CDC) and leaves bcdUSB
// at 2.00: Windows then sees one function, not a composite, never reads the
// BOS, and binds nothing (code 28). Here: a composite with IADs (EF/02/01), and
// 2.10 so the BOS with the MS OS 2.0 descriptor (WinUSB on interface 0, the
// link) is read. Strings 1-3 as the core numbers them.
extern "C" uint8_t const* tud_descriptor_device_cb(void)
{
    static const tusb_desc_device_t d = {
        .bLength = sizeof(tusb_desc_device_t),
        .bDescriptorType = TUSB_DESC_DEVICE,
        .bcdUSB = 0x0210,
        .bDeviceClass = TUSB_CLASS_MISC,
        .bDeviceSubClass = MISC_SUBCLASS_COMMON,
        .bDeviceProtocol = MISC_PROTOCOL_IAD,
        .bMaxPacketSize0 = 64,
        .idVendor = USB_VID,
        .idProduct = USB_PID,
        .bcdDevice = 0x0100,
        .iManufacturer = 0x01,
        .iProduct = 0x02,
        .iSerialNumber = 0x03,
        .bNumConfigurations = 0x01,
    };
    return (uint8_t const*)&d;
}

// TinyUSB, on its own task: data from the host. Wake the link.
extern "C" void tud_vendor_rx_cb(uint8_t itf)
{
    (void)itf;
    UsbPipe::wakeFromRx();
}
#endif
