#!/usr/bin/env python3
"""Talk to the board over its USB link from a PC (docs/design/usb-link.md).

    pip install pyusb libusb-package
    python tools/usb_link.py hello
    python tools/usb_link.py get /api/status
    python tools/usb_link.py bench            # GET / and time it
    python tools/usb_link.py events           # print alert events until ^C
    python tools/usb_link.py test fuel_low    # POST /api/alerts/test, then wait for the event

On Windows the t2can build's WinUSB descriptor (WebUSB on) binds the vendor
interface with no driver install; t2can_usbhid has none (Zadig, or a phone).
The same frames as the app (android/.../UsbLink.kt).
"""
import json
import struct
import sys
import time

import usb.core
import usb.util

try:
    import libusb_package
    BACKEND = libusb_package.get_libusb1_backend()
except ImportError:
    BACKEND = None

VID = 0x303A
HELLO, PING, HTTP_REQ, HTTP_CLOSE = 0x01, 0x02, 0x10, 0x11
HELLO_ACK, PONG, HTTP_RESP, HTTP_FAIL, EVENT = 0x81, 0x82, 0x90, 0x91, 0xA0
FIN = 1


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(t, flags, chan, payload=b""):
    body = struct.pack("<BBHH", t, flags, chan, len(payload)) + payload
    return b"MO" + body + struct.pack("<H", crc16(body))


class Link:
    def __init__(self):
        dev = None
        for d in usb.core.find(find_all=True, idVendor=VID, backend=BACKEND):
            dev = d
            break
        if dev is None:
            sys.exit("no MegaOpen board on USB (VID 303A)")
        self.dev = dev
        cfg = dev.get_active_configuration()
        itf = usb.util.find_descriptor(cfg, bInterfaceClass=0xFF)
        if itf is None:
            sys.exit("the board has no vendor interface (an old build?)")
        try:
            if dev.is_kernel_driver_active(itf.bInterfaceNumber):
                dev.detach_kernel_driver(itf.bInterfaceNumber)
        except (NotImplementedError, usb.core.USBError):
            pass
        usb.util.claim_interface(dev, itf.bInterfaceNumber)
        self.ep_out = usb.util.find_descriptor(itf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        self.ep_in = usb.util.find_descriptor(itf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
        self.buf = b""
        self.bad = 0
        self.events = []

    def send(self, t, flags=FIN, chan=0, payload=b""):
        self.ep_out.write(encode(t, flags, chan, payload), timeout=2000)

    def frames(self, timeout_s):
        """Yield (type, flags, chan, payload) until timeout_s passes with nothing."""
        end = time.time() + timeout_s
        while time.time() < end:
            try:
                self.buf += bytes(self.ep_in.read(4096, timeout=200))
            except usb.core.USBTimeoutError:
                pass
            while True:
                i = self.buf.find(b"MO")
                if i < 0:
                    self.buf = self.buf[-1:]
                    break
                self.buf = self.buf[i:]
                if len(self.buf) < 8:
                    break
                t, flags, chan, n = struct.unpack("<BBHH", self.buf[2:8])
                if n > 1024:
                    self.buf = self.buf[1:]; self.bad += 1; continue
                if len(self.buf) < 10 + n:
                    break
                (crc,) = struct.unpack("<H", self.buf[8 + n:10 + n])
                if crc16(self.buf[2:8 + n]) != crc:
                    self.buf = self.buf[1:]; self.bad += 1; continue
                payload = self.buf[8:8 + n]
                self.buf = self.buf[10 + n:]
                end = time.time() + timeout_s
                yield t, flags, chan, payload

    def hello(self):
        self.send(HELLO, payload=json.dumps({"app": "usb_link.py"}).encode())
        for t, _, _, p in self.frames(2):
            if t == HELLO_ACK:
                return json.loads(p)
        sys.exit("no HELLO reply")

    def request(self, method, path, body=b"", ctype=None, chan=0, timeout_s=10):
        head = "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nX-MegaOpen: 1\r\n" % (method, path)
        if body or method == "POST":
            head += "Content-Length: %d\r\n" % len(body)
            if ctype:
                head += "Content-Type: %s\r\n" % ctype
        data = head.encode() + b"\r\n" + body
        for i in range(0, len(data), 1024):
            last = i + 1024 >= len(data)
            self.send(HTTP_REQ, FIN if last else 0, chan, data[i:i + 1024])
        out = b""
        for t, flags, c, p in self.frames(timeout_s):
            if t == EVENT:
                self.events.append(p)       # pushed while we waited
                continue
            if c != chan and t in (HTTP_RESP, HTTP_FAIL):
                continue
            if t == HTTP_FAIL:
                raise RuntimeError("link FAIL: %s" % p.decode())
            if t == HTTP_RESP:
                out += p
                # The server keeps the connection: the response ends where its
                # framing says. The channel stays open for the next request.
                if flags & FIN or complete(out):
                    return out
        raise RuntimeError("no end of response (%d bytes)" % len(out))

    def close(self, chan=0):
        self.send(HTTP_CLOSE, FIN, chan)


def complete(resp):
    """True once resp holds a whole HTTP response (Content-Length or chunked)."""
    CRLF = bytes([13, 10])
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
    head, _, body = resp.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    hdr = {k.strip().lower(): v.strip() for k, _, v in (l.partition(":") for l in lines[1:])}
    if hdr.get("transfer-encoding", "").lower() == "chunked":
        out, rest = b"", body
        while rest:
            size, _, rest = rest.partition(b"\r\n")
            n = int(size.split(b";")[0] or b"0", 16)
            if n == 0:
                break
            out += rest[:n]
            rest = rest[n + 2:]
        body = out
    return lines[0], hdr, body


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "hello"
    link = Link()
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
        n = 20
        for _ in range(n):
            link.request("GET", "/api/alerts?since=0")
        print("small request round trip: %.0f ms" % ((time.time() - t0) / n * 1000))
        t0 = time.time()
        link.send(PING, payload=b"x")
        for t, *_ in link.frames(2):
            if t == PONG:
                print("ping: %.1f ms" % ((time.time() - t0) * 1000))
                break
    elif cmd == "test":
        sig = sys.argv[2] if len(sys.argv) > 2 else "drive_no_lights"
        print(split(link.request("POST", "/api/alerts/test", json.dumps({"sig": sig}).encode(), "application/json"))[0])
        t0 = time.time()
        while not link.events and time.time() - t0 < 3:
            for t, _, _, p in link.frames(0.5):
                if t == EVENT:
                    link.events.append(p)
                    break
        print("EVENT", link.events[0].decode() if link.events else "none")
    elif cmd == "events":
        while True:
            link.send(PING, payload=b"")
            for t, _, _, p in link.frames(1):
                if t == EVENT:
                    print(time.strftime("%H:%M:%S"), p.decode())
    print("bad frames:", link.bad)


if __name__ == "__main__":
    main()
