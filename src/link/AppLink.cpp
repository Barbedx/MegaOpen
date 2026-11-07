#include "AppLink.h"
#if FEATURE_APP_LINK
#include <Arduino.h>
#include <new>
#include "../app/Rt.h"
#include "../utils/LogBus.h"
#include "../utils/PsHeap.h"

namespace AppLink
{
    namespace
    {
        TaskHandle_t   s_task = nullptr;
        int            s_id = -1;
        // Each session holds ~4.5 kB of frame buffers: PSRAM, not BSS.
        Link::Session* s_usb = nullptr;
        Link::Session* s_ble = nullptr;
        bool         (*s_wifiOnline)() = nullptr;
        // The session the running net transfer went to (one at a time).
        Link::Session* volatile s_net = nullptr;
        Via            s_via = Via::None;

        void wake() { if (s_task) xTaskNotifyGive(s_task); }

        Link::Session* make(Link::Pipe& p, const char* name)
        {
            void* m = PsHeap::malloc(sizeof(Link::Session));
            if (!m) { LOGW("link", "%s: no memory for the session", p.tag()); return nullptr; }
            Link::Session* s = new (m) Link::Session(p);
            s->begin(name);
            return s;
        }

        bool wifiOnline() { return s_wifiOnline && s_wifiOnline(); }

        Via pick()
        {
            if (s_usb && s_usb->alive()) return Via::Usb;
            if (s_ble && s_ble->alive()) return Via::Ble;
            return Via::None;
        }

        void run(void*)
        {
            for (;;) {
                // Woken by data from the app (TinyUSB's rx callback, BLE's
                // write callback); polled faster while a response may be
                // waiting on a socket.
                const bool busy = (s_usb && s_usb->busy()) || (s_ble && s_ble->busy());
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(busy ? 2 : 50));
                const uint32_t t0 = micros();
                const uint32_t now = millis();
#if FEATURE_USB_LINK
                UsbPipe::watch(now);
#endif
                if (s_usb) s_usb->service(now);
                if (s_ble) s_ble->service(now);
                const Via v = pick();
                if (v != s_via) {
                    LOGI("link", "app via %s (was %s)", viaName(v), viaName(s_via));
                    s_via = v;
                }
                App::Tasks::beat(s_id, micros() - t0);
            }
        }
    }

    const char* viaName(Via v)
    {
        switch (v) {
            case Via::Usb: return "usb";
            case Via::Ble: return "ble";
            default:       return "none";
        }
    }

    void begin(const char* name)
    {
#if FEATURE_USB_LINK
        s_usb = make(UsbPipe::begin(wake), name);
#endif
#if FEATURE_BLE_LINK
        s_ble = make(BlePipe::pipe(wake), name);
#endif
        App::TaskSpec spec;
        spec.name  = "applink";
        spec.fn    = run;
        spec.stack = App::Plan::APPLINK_STACK;
        spec.prio  = App::Plan::APPLINK_PRIO;
        spec.core  = App::Plan::APPLINK_CORE;
        spec.idOut = &s_id;
        s_task = App::spawn(spec);
    }

    void setWifiOnline(bool (*online)()) { s_wifiOnline = online; }

    bool alive()    { return pick() != Via::None; }
    bool usbNet()   { return s_usb && s_usb->netAvailable(); }
    Via  via()      { return pick(); }

    void stats(Stats& out)
    {
        out = Stats{};
        out.via = pick();
        if (s_usb) s_usb->stats(out.usb);
        if (s_ble) s_ble->stats(out.ble);
#if FEATURE_USB_LINK
        UsbPipe::bus(out.bus);
#endif
#if FEATURE_BLE_LINK
        BlePipe::stats(out.peer);
#endif
    }

    // ---- the app's internet ----------------------------------------------
    namespace
    {
        Link::Session* netSession()
        {
            if (s_usb && s_usb->netAvailable()) return s_usb;
            if (s_ble && s_ble->netAvailable() && !wifiOnline()) return s_ble;
            return nullptr;
        }
    }

    bool netFirst() { return netSession() != nullptr; }

    bool netAvailable()
    {
        return (s_usb && s_usb->netAvailable()) || (s_ble && s_ble->netAvailable());
    }

    bool netOpen(const char* method, const char* url, const char* const* headers,
                 const uint8_t* body, size_t len, uint32_t timeoutMs, Link::NetHead& head, const char*& why)
    {
        // netFirst's choice, else any app that offers it (the caller asked).
        Link::Session* s = netSession();
        if (!s) s = (s_usb && s_usb->netAvailable()) ? s_usb : (s_ble && s_ble->netAvailable()) ? s_ble : nullptr;
        if (!s) { why = "no_app"; return false; }
        s_net = s;
        return s->netOpen(method, url, headers, body, len, timeoutMs, head, why, wake);
    }

    int netRead(uint8_t* buf, size_t n, uint32_t timeoutMs)
    {
        Link::Session* s = s_net;
        return s ? s->netRead(buf, n, timeoutMs, wake) : -1;
    }

    void netClose()
    {
        Link::Session* s = s_net;
        if (s) s->netClose(wake);
        s_net = nullptr;
    }
}
#endif
