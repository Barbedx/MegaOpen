#!/usr/bin/env python3
"""Talk to the board over its USB link from a PC (docs/design/usb-link.md).

    pip install pyusb libusb-package
    python tools/usb_link.py hello
    python tools/usb_link.py get /api/status
    python tools/usb_link.py bench            # GET / and time it
    python tools/usb_link.py events           # print alert events until ^C
    python tools/usb_link.py test fuel_low    # POST /api/alerts/test, then wait for the event
    python tools/usb_link.py gateway          # be the app's internet for the board, until ^C
    python tools/usb_link.py gateway check    # ... and ask the board to check GitHub for an update

`gateway` does what MegaOpen Companion does for the board: it carries the board's
own GitHub requests (GhOta, LogPush) over this PC's network, GitHub hosts only.

On Windows the t2can build's WinUSB descriptor (WebUSB on) binds the vendor
interface with no driver install; t2can_usbhid has none (Zadig, or a phone).
The same frames as the app (android/.../Link.kt, UsbBoard.kt). Under Git Bash
set MSYS_NO_PATHCONV=1, or /api/... arrives as a Windows path.
"""
import json
import queue
import struct
import sys
import threading
import time
import urllib.error
import urllib.request

import usb.core
import usb.util

try:
    import libusb_package
    BACKEND = libusb_package.get_libusb1_backend()
except ImportError:
    BACKEND = None

VID = 0x303A
HELLO, PING, HTTP_REQ, HTTP_CLOSE = 0x01, 0x02, 0x10, 0x11
NET_HEAD, NET_DATA, NET_FAIL = 0x40, 0x41, 0x42
HELLO_ACK, PONG, HTTP_RESP, HTTP_FAIL, EVENT = 0x81, 0x82, 0x90, 0x91, 0xA0
NET_REQ, NET_BODY, NET_ACK, NET_CANCEL = 0xC0, 0xC1, 0xC2, 0xC3
FIN = 1
NET_WINDOW = 16384
NET_HOSTS = {"api.github.com", "github.com", "objects.githubusercontent.com",
             "release-assets.githubusercontent.com"}
CRLF = bytes([13, 10])


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(t, flags, chan, payload=b""):
    body = struct.pack("<BBHH", t, flags, chan, len(payload)) + payload
    return b"MO" + body + struct.pack("<H", crc16(body))


def complete(resp):
    """True once resp holds a whole HTTP response (Content-Length or chunked)."""
    head, sep, body = resp.partition(CRLF + CRLF)
    if not sep:
        return False
    hdr = {}
    for line in head.decode("latin-1").split(CRLF.decode())[1:]:
        k, _, v = line.partition(":")
        hdr[k.strip().lower()] = v.strip()
    if hdr.get("transfer-encoding", "").lower() == "chunked":
        rest = body
        while True:
            size, sep, rest = rest.partition(CRLF)
            if not sep:
                return False
            n = int(size.split(b";")[0] or b"0", 16)
            if n == 0:
                return rest.startswith(CRLF) or (CRLF + CRLF) in rest
            if len(rest) < n + 2:
                return False
            rest = rest[n + 2:]
    if "content-length" in hdr:
        return len(body) >= int(hdr["content-length"])
    return False


def split(resp):
    head, _, body = resp.partition(CRLF + CRLF)
    lines = head.decode("latin-1").split(CRLF.decode())
    hdr = {k.strip().lower(): v.strip() for k, _, v in (l.partition(":") for l in lines[1:])}
    if hdr.get("transfer-encoding", "").lower() == "chunked":
        out, rest = b"", body
        while rest:
            size, _, rest = rest.partition(CRLF)
            n = int(size.split(b";")[0] or b"0", 16)
            if n == 0:
                break
            out += rest[:n]
            rest = rest[n + 2:]
        body = out
    return lines[0], hdr, body


class Link:
    def __init__(self, net=False):
        dev = next(iter(usb.core.find(find_all=True, idVendor=VID, backend=BACKEND)), None)
        if dev is None:
            sys.exit("no MegaOpen board on USB (VID 303A)")
        cfg = dev.get_active_configuration()
        itf = usb.util.find_descriptor(cfg, bInterfaceClass=0xFF)
        if itf is None:
            sys.exit("the board has no vendor interface (an old build?)")
        usb.util.claim_interface(dev, itf.bInterfaceNumber)
        out_ = lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT
        self.ep_out = usb.util.find_descriptor(itf, custom_match=out_)
        self.ep_in = usb.util.find_descriptor(itf, custom_match=lambda e: not out_(e))
        self.wlock = threading.Lock()
        self.ctl = queue.Queue()
        self.events = queue.Queue()
        self.chans = {}
        self.bad = 0
        self.net = net
        self.job = None
        threading.Thread(target=self._read, daemon=True).start()

    def send(self, t, flags=FIN, chan=0, payload=b""):
        with self.wlock:
            self.ep_out.write(encode(t, flags, chan, payload), timeout=2000)

    def _read(self):
        buf = b""
        while True:
            try:
                buf += bytes(self.ep_in.read(16384, timeout=200))
            except usb.core.USBTimeoutError:
                continue
            except usb.core.USBError:
                return
            while True:
                i = buf.find(b"MO")
                if i < 0:
                    buf = buf[-1:]
                    break
                buf = buf[i:]
                if len(buf) < 8:
                    break
                t, flags, chan, n = struct.unpack("<BBHH", buf[2:8])
                if n > 1024:
                    buf = buf[1:]; self.bad += 1; continue
                if len(buf) < 10 + n:
                    break
                (crc,) = struct.unpack("<H", buf[8 + n:10 + n])
                if crc16(buf[2:8 + n]) != crc:
                    buf = buf[1:]; self.bad += 1; continue
                p = buf[8:8 + n]
                buf = buf[10 + n:]
                self._frame(t, flags, chan, p)

    def _frame(self, t, flags, chan, p):
        if t in (HTTP_RESP, HTTP_FAIL):
            self.chans.setdefault(chan, queue.Queue()).put((t, flags, p))
        elif t == EVENT:
            self.events.put(p)
        elif t in (HELLO_ACK, PONG):
            self.ctl.put((t, p))
        elif t == NET_REQ:
            self.req = (chan, json.loads(p), bytearray())
            if flags & FIN:
                self._net_start()
        elif t == NET_BODY and getattr(self, "req", None) and self.req[0] == chan:
            self.req[2].extend(p)
            if flags & FIN:
                self._net_start()
        elif t == NET_ACK and self.job and self.job["id"] == chan:
            with self.job["cv"]:
                self.job["acked"] = struct.unpack("<I", p[:4])[0]
                self.job["cv"].notify_all()
        elif t == NET_CANCEL and self.job and self.job["id"] == chan:
            with self.job["cv"]:
                self.job["cancel"] = True
                self.job["cv"].notify_all()

    # ---- the board's internet ----------------------------------------------
    def _net_start(self):
        chan, head, body = self.req
        self.req = None
        job = {"id": chan, "acked": 0, "cancel": False, "cv": threading.Condition()}
        self.job = job
        threading.Thread(target=self._net_run, args=(job, head, bytes(body)), daemon=True).start()

    def _net_run(self, job, head, body):
        url, m = head["url"], head.get("m", "GET")
        host = url.split("/")[2] if url.startswith("https://") else ""
        print("  net: %s %s (%d bytes up)" % (m, url[:90], len(body)))
        if host not in NET_HOSTS:
            self.send(NET_FAIL, FIN, job["id"], b"host")
            return
        req = urllib.request.Request(url, data=body or None, method=m)
        for k, v in head.get("h", {}).items():
            req.add_header(k, v)
        req.add_header("Accept-Encoding", "identity")
        try:
            r = urllib.request.urlopen(req, timeout=30)
            code = r.status
        except urllib.error.HTTPError as e:
            r, code = e, e.code
        except Exception as e:
            self.send(NET_FAIL, FIN, job["id"], (type(e).__name__ + " " + str(e))[:36].encode())
            return
        final = r.geturl()
        if final.split("/")[2] not in NET_HOSTS:
            self.send(NET_FAIL, FIN, job["id"], b"host")
            return
        ln = r.headers.get("Content-Length")
        self.send(NET_HEAD, FIN, job["id"], json.dumps({"status": code, "len": int(ln) if ln else -1, "url": final}).encode())
        sent, t0 = 0, time.time()
        while True:
            d = r.read(1024)
            if not d:
                break
            with job["cv"]:
                while not job["cancel"] and sent + len(d) - job["acked"] > NET_WINDOW:
                    if not job["cv"].wait(30):
                        self.send(NET_FAIL, FIN, job["id"], b"stalled")
                        return
            if job["cancel"]:
                print("  net: cancelled by the board at %d bytes" % sent)
                return
            self.send(NET_DATA, 0, job["id"], d)
            sent += len(d)
        self.send(NET_DATA, FIN, job["id"])
        dt = time.time() - t0
        print("  net: %d %d bytes in %.1f s" % (code, sent, dt))

    # ---- control and HTTP ----------------------------------------------------
    def hello(self):
        p = {"app": "usb_link.py"}
        if self.net:
            p["net"] = 1
        self.send(HELLO, payload=json.dumps(p).encode())
        try:
            t, p = self.ctl.get(timeout=3)
        except queue.Empty:
            sys.exit("no HELLO reply")
        return json.loads(p)

    def request(self, method, path, body=b"", ctype=None, chan=0, timeout_s=10):
        head = "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nX-MegaOpen: 1\r\n" % (method, path)
        if body or method == "POST":
            head += "Content-Length: %d\r\n" % len(body)
            if ctype:
                head += "Content-Type: %s\r\n" % ctype
        data = head.encode() + CRLF + body
        q = self.chans.setdefault(chan, queue.Queue())
        for i in range(0, len(data), 1024):
            self.send(HTTP_REQ, FIN if i + 1024 >= len(data) else 0, chan, data[i:i + 1024])
        out = b""
        end = time.time() + timeout_s
        while time.time() < end:
            try:
                t, flags, p = q.get(timeout=0.2)
            except queue.Empty:
                continue
            if t == HTTP_FAIL:
                raise RuntimeError("link FAIL: %s" % p.decode())
            out += p
            # The server keeps the connection: the response ends where its
            # framing says. The channel stays open for the next request.
            if flags & FIN or complete(out):
                return out
        raise RuntimeError("no end of response (%d bytes)" % len(out))

    def get_json(self, path):
        return json.loads(split(self.request("GET", path))[2])

    def post_json(self, path, obj):
        return split(self.request("POST", path, json.dumps(obj).encode(), "application/json"))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "hello"
    link = Link(net=cmd == "gateway")
    info = link.hello()
    print("HELLO", info)
    if cmd == "get":
        status, hdr, body = split(link.request("GET", sys.argv[2]))
        print(status)
        print(body.decode("utf-8", "replace")[:4000])
    elif cmd == "bench":
        t0 = time.time()
        status, hdr, body = split(link.request("GET", "/", timeout_s=30))
        dt = time.time() - t0
        print(status, "%d bytes in %.2f s = %.0f kB/s" % (len(body), dt, len(body) / dt / 1024))
        t0 = time.time()
        for _ in range(20):
            link.request("GET", "/api/alerts?since=0")
        print("small request round trip: %.0f ms" % ((time.time() - t0) / 20 * 1000))
        t0 = time.time()
        link.send(PING, payload=b"x")
        link.ctl.get(timeout=2)
        print("ping: %.1f ms" % ((time.time() - t0) * 1000))
    elif cmd == "test":
        sig = sys.argv[2] if len(sys.argv) > 2 else "drive_no_lights"
        print(link.post_json("/api/alerts/test", {"sig": sig})[0])
        try:
            print("EVENT", link.events.get(timeout=3).decode())
        except queue.Empty:
            print("EVENT none")
    elif cmd == "events":
        while True:
            link.send(PING, payload=b"")
            try:
                print(time.strftime("%H:%M:%S"), link.events.get(timeout=1).decode())
            except queue.Empty:
                pass
    elif cmd == "watch":
        # The board's status over the link every 5 s, for N s (WiFi may be off).
        end = time.time() + (int(sys.argv[2]) if len(sys.argv) > 2 else 60)
        while time.time() < end:
            st = link.get_json("/api/status")
            print(time.strftime("%H:%M:%S"), "wifi", st.get("mode"), "heap", st.get("heap"), "up", st.get("uptimeMs"))
            for _ in range(5):
                link.send(PING, payload=b"")
                time.sleep(1)
    elif cmd == "gateway":
        what = sys.argv[2] if len(sys.argv) > 2 else ""
        if what == "check":
            print("check:", link.post_json("/api/fw/check", {})[0])
        while True:
            link.send(PING, payload=b"")
            time.sleep(1)
            if what == "check":
                fw = link.get_json("/api/fw")
                print("  fw:", {k: fw.get(k) for k in ("phase", "latest", "err", "got", "size")})
                if fw.get("phase") not in ("checking", "downloading", "verifying"):
                    break
    print("bad frames:", link.bad)


if __name__ == "__main__":
    main()
