#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../Features.h"

// One app on one transport (docs/design/usb-link.md): the frames, HTTP piped
// to the board's own web server on 127.0.0.1, the alerts pushed as events,
// the app's internet. What moves the bytes is a Pipe: the USB vendor
// interface (UsbPipe) or a BLE GATT service (BlePipe). Every session is
// served by the one "applink" task (AppLink.cpp); only the net calls come
// from another task (NetJob).
namespace Link
{
    // Two requests at a time: each holds two of the web server's four sockets.
    constexpr uint8_t  CHANS = 2;
    constexpr uint32_t IDLE_MS = 15000;    // a channel this quiet is closed
    constexpr uint32_t ALIVE_MS = 3000;    // a frame this recent: the app is there
    // The app's internet: 16 kB of credit at a time.
    constexpr size_t   NET_WINDOW = 16384;

    struct NetHead {
        int     status = 0;
        int64_t len = -1;          // -1: the app does not know
        char    url[512] = {0};    // where it ended up, after the redirects
    };

    // What every transport counts the same way.
    struct SessionStats {
        bool     up = false;          // the transport has a host / peer
        bool     alive = false;       // the app sent a frame in the last ALIVE_MS
        bool     hello = false;       // ... and said HELLO since it came up
        uint8_t  open = 0;            // channels with a socket
        uint32_t rxFrames = 0, txFrames = 0, bad = 0;
        uint32_t reqs = 0, fails = 0, events = 0;
        uint32_t hellos = 0, helloAckFails = 0;   // HELLOs in, HELLO_ACKs that did not go out
        uint32_t rxBytes = 0, txBytes = 0;
        uint32_t lastRxAgoMs = 0;     // UINT32_MAX: never
        // The longest stall, ms, counted across applink passes: for room in
        // the transport, connect(), a socket send(). And frames given up
        // half-written / not sent (the app gone quiet, a full control queue).
        uint32_t txWaitMax = 0, connectMax = 0, sockSendMax = 0, cut = 0, dropped = 0;
    };

}

#if FEATURE_APP_LINK
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "LinkFrame.h"

namespace Link
{
    // The bytes' way. Every call but the constructor's on the applink task;
    // none waits (R2: no applink pass blocks, car 2026-11-22 `applink
    // busy=3597703us` in a 10 s window).
    class Pipe {
    public:
        virtual ~Pipe() = default;
        virtual const char* tag() const = 0;    // "usb", "ble": the log's and the API's name
        virtual bool up() = 0;                  // a host / a subscribed peer is there
        virtual size_t read(uint8_t* buf, size_t n) = 0;   // what is there
        // A frame may start now (USB: the FIFO is empty, LinkFrame.h usbFiller).
        virtual bool ready() { return true; }
        // Bytes appended to a frame of n (USB: the filler, 0x00).
        virtual size_t pad(size_t n) const { (void)n; return 0; }
        // As much of f as there is room for now; the bytes taken.
        virtual size_t write(const uint8_t* f, size_t n) = 0;
        // A frame stalled while the app keeps writing is given up after
        // TX_RX_WAITING_MS (USB: a host that writes and does not read).
        virtual bool cutWhileTalking() const { return false; }
    };

    class Session {
    public:
        explicit Session(Pipe& pipe) : _pipe(pipe) {}
        const char* tag() const { return _pipe.tag(); }

        // ---- the applink task --------------------------------------------
        void begin(const char* name) { _name = name ? name : ""; }
        void service(uint32_t now);
        bool busy() const;                      // a channel is open: poll fast
        bool txBusy() const { return !txIdle(); }   // a frame waits for room: poll faster

        // ---- any task ----------------------------------------------------
        bool alive() const;                     // up, and a frame lately
        bool helloDone() const { return _hello; }
        bool netAvailable() const { return _hello && _appNet && alive(); }
        void stats(SessionStats& out) const;

        // The board's own GitHub requests carried by the app (AppLink.h);
        // one at a time, any task but applink.
        bool netOpen(const char* method, const char* url, const char* const* headers,
                     const uint8_t* body, size_t len, uint32_t timeoutMs, NetHead& head,
                     const char*& why, void (*wake)());
        int  netRead(uint8_t* buf, size_t n, uint32_t timeoutMs, void (*wake)());
        void netClose(void (*wake)());

    private:
        enum class NetSt : uint8_t { Idle, Send, Head, Body, Fail };
        static constexpr uint32_t NET_ACK_STEP = 4096;
        static constexpr uint32_t TX_GIVE_UP_MS = 3000;
        static constexpr uint32_t TX_GONE_MS = 20;
        // While the host is writing to us and not reading: its frames (a new
        // app's HELLO) come first.
        static constexpr uint32_t TX_RX_WAITING_MS = 500;
        // A request body (an OTA upload) waits on the server reading it, and
        // connect() on the server's accept: this long without progress, then
        // the channel fails. Counted across passes, never waited in one.
        static constexpr uint32_t SOCK_GIVE_UP_MS = 5000;
        static constexpr uint32_t EVENT_MS = 200;
        static constexpr size_t   RX_CHUNK = 256;
        static constexpr uint8_t  CTL_SLOTS = 4;
        static constexpr uint8_t  TX_FRAMES_PASS = 8;

        // A channel's request bytes are its own: the parser's buffer is
        // reused by the next frame. While any are unsent the pipe is not
        // read (USB NAKs, BLE fills its buffer), so one frame's payload plus
        // what was left of that read is the most a channel holds.
        struct Chan {
            int      fd = -1;
            bool     connecting = false;
            uint32_t lastMs = 0;
            uint32_t since = 0;           // connect() started / the last bytes the server took
            uint16_t reqN = 0, reqAt = 0;
            uint8_t  req[MAX_PAYLOAD + RX_CHUNK];
        };
        // One frame on its way out: the bytes (and the USB filler), how far.
        struct Out {
            uint16_t n = 0, at = 0;       // n 0: empty
            uint8_t  type = 0, flags = 0;
            uint16_t chan = 0;
            uint8_t  buf[MAX_FRAME + 1];
        };

        bool hostLive() const;
        bool fill(Out& o, uint8_t type, uint8_t flags, uint16_t chan, const uint8_t* p, size_t len);
        bool control(uint8_t type, uint8_t flags, uint16_t chan, const uint8_t* p, size_t len);
        bool controlText(uint8_t type, uint16_t chan, const char* s);
        bool txIdle() const { return !_cur && !_ctlN && !_data.n; }
        void txService();
        bool txStalled(uint32_t t);
        void txDone(bool ok);
        bool nextData(uint32_t now);
        void closeChan(uint16_t c);
        void fail(uint16_t c, const char* why);
        void closeAll();
        bool openChan(uint16_t c);
        bool reqWaiting() const;
        void sockService(uint32_t now);
        void reqFlush(uint16_t c, uint32_t now);
        void onRequest(const Frame& f);
        bool pumpOne(uint32_t now);
        bool eventOne(uint32_t now);
        bool netOne();
        void netFail(const char* why);
        void netService();
        void onNet(const Frame& f);
        void hello(const Frame& f);
        void onFrame(const Frame& f);
        void down();
        static void frameCb(const Frame& f, void* ctx) { static_cast<Session*>(ctx)->onFrame(f); }

        Pipe&        _pipe;
        const char*  _name = "";
        Parser       _parser;
        Chan         _ch[CHANS];
        uint8_t      _rx[RX_CHUNK];
        uint8_t      _body[MAX_PAYLOAD];
        // Out: one data frame (HTTP, an event, the net request) and a few
        // control frames (PONG, HELLO_ACK, NET_ACK, NET_CANCEL, 91), sent at
        // frame boundaries before the data.
        Out          _data;
        Out          _ctl[CTL_SLOTS];
        uint8_t      _ctlHead = 0, _ctlN = 0;
        Out*         _cur = nullptr;      // the frame being written
        uint32_t     _txSince = 0;        // its last progress
        uint32_t     _txRx = 0;           // rxFrames then
        uint8_t      _rr = 0;             // the data producer to ask first
        uint8_t      _pumpC = 0;          // the channel to read first
        bool         _up = false;
        bool         _hello = false;
        bool         _evMore = false;     // the last ask found an alert: ask again now
        uint32_t     _evSeq = 0;
        uint32_t     _evAt = 0;
        volatile uint32_t _lastRx = 0;
        volatile bool     _anyRx = false;
        SessionStats _st;

        // ---- the app's internet: one transfer at a time --------------------
        // The requester fills the request and sets Send; applink sends it and
        // moves the state on as the app answers; the body comes through a
        // stream buffer, the requester's to read. Every transfer has its own
        // number (the frames' chan), so a late frame of a cancelled one is
        // dropped.
        volatile NetSt     _netSt = NetSt::Idle;
        uint16_t           _netId = 0;
        char               _netReq[MAX_PAYLOAD];
        size_t             _netReqLen = 0;
        const uint8_t*     _netBody = nullptr;
        size_t             _netBodyLen = 0;
        size_t             _netAt = 0;        // body bytes framed so far (applink's)
        bool               _netReqOut = false; // the NET_REQ frame made (applink's)
        NetHead            _netHead;
        char               _netWhy[40] = {0};
        StreamBufferHandle_t _netSb = nullptr;
        StaticStreamBuffer_t _netSbStruct;
        volatile uint32_t  _netTaken = 0;     // bytes the requester read
        uint32_t           _netAcked = 0;     // what the app was last told
        volatile bool      _netEof = false;
        volatile bool      _netCancel = false;
        volatile uint16_t  _netCancelId = 0;  // the transfer the cancel is for
        bool               _appNet = false;   // the app's HELLO offered it
    };
}
#endif
