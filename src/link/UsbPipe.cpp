#include "UsbPipe.h"
#if FEATURE_USB_LINK
#include <Arduino.h>
#include <string.h>
#include "esp32-hal-tinyusb.h"
#include "USB.h"
#include "soc/usb_dwc_struct.h"
#include "device/usbd_pvt.h"
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
        uint8_t s_epOut = 0;   // the OUT endpoint's address (IN is 0x80 | it)

        uint16_t loadDescriptor(uint8_t* dst, uint8_t* itf)
        {
            const uint8_t str = tinyusb_add_string_descriptor("MegaOpen link");
            const uint8_t ep = tinyusb_get_free_duplex_endpoint();
            TU_VERIFY(ep != 0);
            s_epOut = ep;
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
        // The TX FIFO is one packet: all room means empty.
        constexpr uint32_t FIFO = CFG_TUD_VENDOR_TX_BUFSIZE;
        static_assert(FIFO == Link::USB_PACKET, "the filler assumes a one-packet FIFO");

        void (*s_wake)() = nullptr;
        BusStats s_bus;
        bool     s_busOn = false;        // SOFs lately
        bool     s_sofSeen = false;
        uint32_t s_sof = 0;              // the last DSTS.SOFFN read
        uint32_t s_sofAt = 0;
        // The event counts watch() has said (written by the core's event task).
        uint32_t s_saidUnmounts = 0, s_saidSuspends = 0, s_saidResumes = 0;

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
            // A frame starts on an empty FIFO: the driver takes the FIFO whole
            // into one packet, so the frame's packets start at its first byte
            // and the filler (pad, Link::usbFiller) makes its last one short.
            bool ready() override
            {
                if (tud_vendor_n_write_available(ITF) >= FIFO) return true;
                tud_vendor_n_write_flush(ITF);
                return false;
            }
            size_t pad(size_t n) const override { return Link::usbFiller(n); }
            // What fits in the FIFO now. Never waits: the session goes on next
            // pass, woken by tud_vendor_tx_cb when a packet went out.
            size_t write(const uint8_t* f, size_t n) override
            {
                if (!tud_vendor_n_mounted(ITF)) return 0;
                const uint32_t room = tud_vendor_n_write_available(ITF);
                const uint32_t w = room ? tud_vendor_n_write(ITF, f, n < room ? n : room) : 0;
                tud_vendor_n_write_flush(ITF);
                return w;
            }
            bool cutWhileTalking() const override { return true; }
            // The usbd task is not pinned (esp32-hal-tinyusb.c:718) and arms
            // the endpoints as we do, through usbd_edpt_claim; when both claim
            // at once one gives up and nobody arms. OUT: the packet that
            // filled the 64 B FIFO does not re-arm it, our read does, and a
            // lost claim there left OUT NAKing for good (bench 2026-11-22,
            // `usb_link.py cable`: rx stopped at 1566 frames, no rx callback
            // after). IN: the last frame's tail stayed in the FIFO (2 echoes
            // lost). A zero-byte read re-arms OUT when the FIFO has room (a
            // busy endpoint refuses the claim, harmless); a flush sends a
            // tail.
            // Again next tick while OUT is still not armed with an empty FIFO,
            // or a tail waits with IN idle: a lost claim costs a tick, not
            // the 50 ms poll.
            bool kick() override
            {
                if (!tud_vendor_n_mounted(ITF)) return false;
                uint8_t none;
                const bool empty = !tud_vendor_n_available(ITF);
                if (empty) tud_vendor_n_read(ITF, &none, 0);
                if (tud_vendor_n_write_available(ITF) < FIFO) tud_vendor_n_write_flush(ITF);
                // A busy IN completes and flushes by itself (a host that
                // stopped reading must not keep us polling every tick).
                const bool tail = tud_vendor_n_write_available(ITF) < FIFO;
                return (empty && !usbd_edpt_busy(0, s_epOut)) || (tail && !usbd_edpt_busy(0, (uint8_t)(0x80 | s_epOut)));
            }
        } s_pipe;
    }

    Link::Pipe& begin(void (*wake)())
    {
        s_wake = wake;
        // The frame number as it stands: a change from here on is a host.
        s_sof = USB_DWC.dsts_reg.soffn;
        // What the host does to the bus, for a link that drops: the core's
        // event loop calls this (tud_mount/umount/suspend/resume_cb). Counts
        // only: that task's stack is 2 kB (ARDUINO_SERIAL_EVENT_TASK_STACK_SIZE),
        // and a LOGI there overflowed it on every replug (car boots 289/290,
        // 2026-11-22, task arduino_usb_eve). watch() says it, on applink.
        USB.onEvent([](void*, esp_event_base_t, int32_t id, void*) {
            switch (id) {
                case ARDUINO_USB_STARTED_EVENT: ++s_bus.mounts;   break;
                case ARDUINO_USB_STOPPED_EVENT: ++s_bus.unmounts; break;
                case ARDUINO_USB_SUSPEND_EVENT: ++s_bus.suspends; break;
                case ARDUINO_USB_RESUME_EVENT:  ++s_bus.resumes;  break;
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
        // The core's event task wrote these; one word each, read whole.
        const uint32_t un = *(volatile uint32_t*)&s_bus.unmounts;
        const uint32_t su = *(volatile uint32_t*)&s_bus.suspends;
        const uint32_t re = *(volatile uint32_t*)&s_bus.resumes;
        if (un != s_saidUnmounts) { LOGI("usb", "host: bus reset or unplugged (x%lu)", (unsigned long)(un - s_saidUnmounts)); s_saidUnmounts = un; }
        if (su != s_saidSuspends) { LOGI("usb", "host: bus suspended (x%lu)", (unsigned long)(su - s_saidSuspends)); s_saidSuspends = su; }
        if (re != s_saidResumes)  { LOGI("usb", "host: bus resumed (x%lu)", (unsigned long)(re - s_saidResumes)); s_saidResumes = re; }
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

    void wakeFromRx() { if (s_wake) s_wake(); }   // rx and tx callbacks alike
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

// TinyUSB, on its own task: a packet went out, the FIFO has room. Wake the
// link (notify only: a foreign context, R1).
extern "C" void tud_vendor_tx_cb(uint8_t itf, uint32_t sent)
{
    (void)itf;
    (void)sent;
    UsbPipe::wakeFromRx();
}
#endif
