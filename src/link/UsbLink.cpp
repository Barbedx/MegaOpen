#include "UsbLink.h"
#if FEATURE_USB_LINK
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include "esp32-hal-tinyusb.h"
#include "lwip/sockets.h"
#include "LinkFrame.h"
#include "../Version.h"
#include "../app/CarTask.h"
#include "../app/Rt.h"
#include "../utils/LogBus.h"
#include "../vehicle/Signals.h"

namespace UsbLink
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
        constexpr uint32_t TX_GIVE_UP_MS = 500; // a host that stops reading
        constexpr uint32_t EVENT_MS = 200;

        struct Chan {
            int      fd = -1;
            uint32_t lastMs = 0;
        };

        TaskHandle_t  s_task = nullptr;
        int           s_id = -1;
        const char*   s_name = "";
        Link::Parser  s_parser;
        Chan          s_ch[CHANS];
        // The one writer's buffers: only the usblink task touches them.
        uint8_t       s_rx[256];
        uint8_t       s_tx[Link::MAX_FRAME];
        uint8_t       s_body[Link::MAX_PAYLOAD];
        bool          s_mounted = false;
        bool          s_hello = false;
        uint32_t      s_evSeq = 0;
        uint32_t      s_evAt = 0;

        // Read by other tasks: single writer (usblink), torn reads cost a number.
        volatile uint32_t s_lastRx = 0;
        volatile bool     s_anyRx = false;
        Stats             s_st;

        // ---- out ----------------------------------------------------------
        // The whole frame into the FIFO, waiting while the host reads. False:
        // unplugged, or the host stopped reading (the frame is cut; the app's
        // parser resyncs).
        bool send(uint8_t type, uint8_t flags, uint16_t chan, const uint8_t* p, size_t len)
        {
            const size_t n = Link::encode(type, flags, chan, p, len, s_tx);
            if (!n) return false;
            size_t at = 0;
            uint32_t since = millis();
            while (at < n) {
                if (!tud_vendor_n_mounted(ITF)) return false;
                const uint32_t room = tud_vendor_n_write_available(ITF);
                if (!room) {
                    tud_vendor_n_write_flush(ITF);
                    if (millis() - since > TX_GIVE_UP_MS) return false;
                    vTaskDelay(1);
                    continue;
                }
                const uint32_t w = tud_vendor_n_write(ITF, s_tx + at, n - at < room ? n - at : room);
                at += w;
                if (w) since = millis();
            }
            tud_vendor_n_write_flush(ITF);
            ++s_st.txFrames;
            s_st.txBytes += n;
            return true;
        }

        bool sendText(uint8_t type, uint16_t chan, const char* s)
        {
            return send(type, Link::FIN, chan, (const uint8_t*)s, strlen(s));
        }

        // ---- channels -------------------------------------------------------
        void closeChan(uint16_t c)
        {
            if (c >= CHANS || s_ch[c].fd < 0) return;
            ::close(s_ch[c].fd);
            s_ch[c].fd = -1;
        }

        void fail(uint16_t c, const char* why)
        {
            closeChan(c);
            ++s_st.fails;
            sendText(Link::HTTP_FAIL, c, why);
        }

        void closeAll()
        {
            for (uint16_t c = 0; c < CHANS; ++c) closeChan(c);
        }

        bool openChan(uint16_t c)
        {
            const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (fd < 0) return false;
            struct sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_port = htons(80);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            // A request body (an OTA upload) waits on the server reading it:
            // at most this long a send, then the channel fails.
            struct timeval tv = {5, 0};
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (::connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) {
                ::close(fd);
                return false;
            }
            s_ch[c].fd = fd;
            s_ch[c].lastMs = millis();
            ++s_st.reqs;
            return true;
        }

        void onRequest(const Link::Frame& f)
        {
            const uint16_t c = f.chan;
            if (c >= CHANS) { sendText(Link::HTTP_FAIL, c, "busy"); ++s_st.fails; return; }
            if (s_ch[c].fd < 0 && !openChan(c)) { fail(c, "connect"); return; }
            size_t at = 0;
            while (at < f.len) {
                const int w = ::send(s_ch[c].fd, f.payload + at, f.len - at, 0);
                if (w <= 0) { fail(c, "send"); return; }
                at += (size_t)w;
            }
            s_ch[c].lastMs = millis();
        }

        // Server to app, as much as there is now; the server closing is FIN.
        void pump(uint32_t now)
        {
            for (uint16_t c = 0; c < CHANS; ++c) {
                if (s_ch[c].fd < 0) continue;
                for (int i = 0; i < 4; ++i) {   // up to 4 kB a pass, then the others
                    const int r = ::recv(s_ch[c].fd, s_body, sizeof(s_body), MSG_DONTWAIT);
                    if (r > 0) {
                        s_ch[c].lastMs = now;
                        if (!send(Link::HTTP_RESP, 0, c, s_body, (size_t)r)) { closeChan(c); break; }
                        continue;
                    }
                    if (r == 0) {
                        send(Link::HTTP_RESP, Link::FIN, c, nullptr, 0);
                        closeChan(c);
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        fail(c, "recv");
                    } else if ((int32_t)(now - s_ch[c].lastMs) > (int32_t)IDLE_MS) {   // signed: a request may have opened it after `now`
                        fail(c, "timeout");
                    }
                    break;
                }
            }
        }

        // ---- control ------------------------------------------------------
        void hello()
        {
            JsonDocument d;
            d["proto"] = 1;
            d["fw"] = MEGAOPEN_VERSION;
            d["build"] = MegaOpen::buildId();
            d["name"] = s_name;
            d["chans"] = CHANS;
            d["upMs"] = millis();
            char out[256];
            const size_t n = serializeJson(d, out, sizeof(out));
            // Alerts from now on; what came before the app was listening is not said.
            static App::Car::AlertEntry none[1];
            App::Car::alertsSince(0, none, 0, s_evSeq);
            closeAll();     // a new HELLO is a new app: its old requests are gone
            s_hello = true;
            send(Link::HELLO_ACK, Link::FIN, 0, (const uint8_t*)out, n);
        }

        void onFrame(const Link::Frame& f, void*)
        {
            ++s_st.rxFrames;
            s_lastRx = millis();
            s_anyRx = true;
            switch (f.type) {
                case Link::HELLO:      hello(); break;
                case Link::PING:       send(Link::PONG, Link::FIN, f.chan, f.payload, f.len); break;
                case Link::HTTP_REQ:   onRequest(f); break;
                case Link::HTTP_CLOSE: closeChan(f.chan); break;
                default: break;        // an app newer than us: ignored
            }
        }

        void events(uint32_t now)
        {
            if (!s_hello || now - s_evAt < EVENT_MS) return;
            s_evAt = now;
            static App::Car::AlertEntry list[App::Car::ALERT_LOG];
            uint32_t last = 0;
            const uint8_t n = App::Car::alertsSince(s_evSeq, list, App::Car::ALERT_LOG, last);
            if (last < s_evSeq) s_evSeq = 0;   // cannot happen without a reboot; be safe
            for (uint8_t i = 0; i < n; ++i) {
                JsonDocument d;
                d["t"] = "alert";
                d["seq"] = list[i].seq;
                d["sig"] = Vehicle::meta(list[i].sig).name;
                d["value"] = list[i].value;
                d["text"] = list[i].text;
                char out[192];
                const size_t len = serializeJson(d, out, sizeof(out));
                if (send(Link::EVENT, Link::FIN, 0, (const uint8_t*)out, len)) ++s_st.events;
                s_evSeq = list[i].seq;
            }
        }

        void unplugged()
        {
            closeAll();
            s_parser.reset();
            s_hello = false;
        }

        void run(void*)
        {
            for (;;) {
                // Woken by data from the host (tud_vendor_rx_cb); polled faster
                // while a response may be waiting on a socket.
                bool busy = false;
                for (uint16_t c = 0; c < CHANS; ++c) busy |= s_ch[c].fd >= 0;
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(busy ? 2 : 50));
                const uint32_t t0 = micros();
                const uint32_t now = millis();
                const bool m = tud_vendor_n_mounted(ITF);
                if (m != s_mounted) {
                    s_mounted = m;
                    LogBus::printf("[usb] link %s", m ? "up: a host configured the device" : "down");
                    if (!m) unplugged();
                }
                if (m) {
                    uint32_t avail;
                    while ((avail = tud_vendor_n_available(ITF)) > 0) {
                        const uint32_t r = tud_vendor_n_read(ITF, s_rx, avail < sizeof(s_rx) ? avail : sizeof(s_rx));
                        if (!r) break;
                        s_st.rxBytes += r;
                        s_parser.feed(s_rx, r, onFrame, nullptr);
                    }
                    pump(now);
                    events(now);
                }
                uint8_t open = 0;
                for (uint16_t c = 0; c < CHANS; ++c) open += s_ch[c].fd >= 0;
                s_st.open = open;
                s_st.mounted = m;
                s_st.hello = s_hello;
                s_st.bad = s_parser.bad();
                App::Tasks::beat(s_id, micros() - t0);
            }
        }
    }

    void begin(const char* name)
    {
        s_name = name ? name : "";
        App::TaskSpec spec;
        spec.name  = "usblink";
        spec.fn    = run;
        spec.stack = App::Plan::USBLINK_STACK;
        spec.prio  = App::Plan::USBLINK_PRIO;
        spec.core  = App::Plan::USBLINK_CORE;
        spec.idOut = &s_id;
        s_task = App::spawn(spec);
    }

    bool alive()
    {
        return s_anyRx && tud_vendor_n_mounted(ITF) && millis() - s_lastRx < ALIVE_MS;
    }

    void stats(Stats& out)
    {
        out = s_st;
        out.alive = alive();
        out.lastRxAgoMs = s_anyRx ? millis() - s_lastRx : UINT32_MAX;
    }
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
    if (UsbLink::s_task) xTaskNotifyGive(UsbLink::s_task);
}
#endif
