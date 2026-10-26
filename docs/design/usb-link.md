# The USB link (1.6)

The board's USB-C port, plugged into the head unit, carries what WiFi carries
today: the settings page, the alerts for the voice app, later the board's way to
the internet (log push, updates). WiFi then becomes the fallback, on only when
no app is on the other end. The canbox UART stays: DUDU's own car screens read it
(com.syu.ms), and nothing else can feed them.

Owner's call, 2026-10-25: all of it ships as 1.6, in one release.

## The device

`ARDUINO_USB_MODE=0` (TinyUSB) in every T-2CAN env. One composite device:

| interface | what | who |
| --- | --- | --- |
| CDC | the serial console, esptool's reset | a PC |
| vendor (class 0xFF, two bulk endpoints, 64 B) | the link | MegaOpen Voice |
| HID (t2can_usbhid only) | media keys | the head unit |

Vendor, not a second CDC: Android hands a vendor interface to the app that asks
for it and nothing else claims it, where a head unit's own services may grab a
serial port. On `t2can` WebUSB is on, which also gives the vendor interface
(interface 0 there) the MS OS 2.0 WinUSB descriptor: a Windows PC talks to it
with no driver install (`tools/usb_link.py`). On `t2can_usbhid` interface 0 is
the HID, so WebUSB stays off.

Two things in the core needed working round, both in `src/link/UsbLink.cpp` and
`variants/t2can/`:

- **The PID.** The core's esp32s3 variant fixes `USB_PID` at 0x1001, the S3's
  USB-Serial-JTAG. Windows kept that device's binding for the new descriptors
  (status Error). `variants/t2can/pins_arduino.h` is the same file with 0x80C3.
  That is not an Espressif allocation; an open-source PID is to be requested.
- **The device class.** With WebUSB on, the core makes the whole device class 02
  (CDC) and leaves bcdUSB at 2.00: one function, not a composite, and the BOS is
  never read (code 28). `tud_descriptor_device_cb` (weak in the core) is ours:
  EF/02/01 with IADs and bcdUSB 2.10. Windows then shows "USB Composite
  Device", "MegaOpen link" (WinUSB) and a COM port.

The link reads TinyUSB's vendor FIFO itself (`tud_vendor_n_read`), not through
the core's `USBVendor`, whose byte queue drops what does not fit. A full FIFO
NAKs the host instead: flow control, nothing lost.

## Frames

Both directions, little-endian:

| bytes | field |
| --- | --- |
| 0-1 | magic `4D 4F` ("MO") |
| 2 | type |
| 3 | flags: bit 0 FIN, the last frame of a message |
| 4-5 | chan: a request's number, 0 for control |
| 6-7 | len, 0..1024 |
| 8.. | payload |
| +2 | CRC-16/CCITT-FALSE over type..payload |

A bad CRC or magic drops one byte and looks for the magic again
(`src/link/LinkFrame.*`, host-tested in `test/test_link`).

| type | way | payload |
| --- | --- | --- |
| `01` HELLO | app → board | JSON `{"app":"1.6.0"}` |
| `81` HELLO | board → app | JSON `{"proto":1,"fw","build","name","chans"}` |
| `02` PING | app → board | anything; echoed |
| `82` PONG | board → app | the PING's payload |
| `10` HTTP | app → board | raw HTTP/1.1 request bytes for `chan`; FIN on the last |
| `11` CLOSE | app → board | give up `chan` |
| `90` HTTP | board → app | raw response bytes; FIN when the server closed |
| `91` FAIL | board → app | `chan` failed: `busy`, `connect`, `timeout`, `send` |
| `A0` EVENT | board → app | JSON, `{"t":"alert",seq,sig,value,text}` |
| `C0` NET_REQ | board → app | JSON `{"m","url","h":{..},"len"}`; FIN when no body |
| `C1` NET_BODY | board → app | the request body, FIN on the last |
| `C2` NET_ACK | board → app | u32: body bytes the board has taken (the credit) |
| `C3` NET_CANCEL | board → app | stop sending |
| `40` NET_HEAD | app → board | JSON `{"status","len","url"}` (the URL after redirects) |
| `41` NET_DATA | app → board | the response body; FIN at the end |
| `42` NET_FAIL | app → board | why there is no answer |

The high bit of the type is the direction. NET frames carry the transfer's
number in `chan`; a frame of an earlier transfer is dropped.

## HTTP over the link

The board does not parse a request: it opens a TCP connection to its own web
server on `127.0.0.1:80` and pipes the bytes both ways. Every route, the request
gate and the settings lock work as they do over WiFi, with nothing duplicated.
The app sends `Host: 127.0.0.1` (an IP literal passes the gate). esp_http_server
keeps the connection open whatever `Connection` says, so the response ends where
its framing says (Content-Length, or chunked's last chunk): the app parses it and
then sends the next request on the same channel, or `11 CLOSE`. The server
closing (FIN) also ends one.

The server's sockets have TCP_NODELAY (`WebUi::begin`): httpd writes the headers
and the body separately, and Nagle held the body for the ACK, which lwIP as the
loopback peer delays: 250 ms a small reply, 7 ms without.

Two channels at a time (`UsbLink::CHANS`): the server keeps 4 sockets and each
link request holds two (both ends of the loopback). A third request gets `91
busy` and the app queues it. A channel quiet for 15 s is closed (`91 timeout`).

Measured on the bench (PC, `tools/usb_link.py bench`): the page (66 kB) at
100 kB/s, a small request 7 ms round trip, a ping 1 ms.

## Alerts

After a HELLO the board sends each new alert as an `A0` event, from the same
ring as `/api/alerts`. What was raised before the HELLO is not sent.

## Net: the app's internet

The board's own GitHub requests, the update check and download (`GhOta`) and the
log push (`LogPush`), go through the app when it is on the link and said
`"net":1` in its HELLO; over WiFi otherwise. The board composes each request as
it does for WiFi, Authorization header and all; the app makes it with Android's
TLS and moves the bytes. It goes to GitHub over https only (the board's
`GhRelease::trustedUrl` list, on the app's side too), follows the redirects
itself and says where it ended up; the board checks that URL again.

One transfer at a time (TlsJob already serialises GhOta and LogPush). The
response body goes into a 16 kB stream buffer in PSRAM, and the app may send no
more than 16 kB past the board's last NET_ACK: a 1.7 MB image cannot outrun the
flash writes.

Measured on the bench with `tools/usb_link.py gateway` playing the app: the
update check through it (the release JSON, 20 kB, in 0.2 s), then the install:
SHA256SUMS, the 302 to the CDN, 1 669 216 bytes in 24 s (64 kB/s), SHA-256
checked, the image booted and passed its OTA trial.

## Host alive

Any frame from the app marks the host alive; the app pings every second. Alive
means a frame within 3 s. The WiFi policy (later in 1.6) reads that.

## Cost

TinyUSB (its task, the CDC and vendor FIFOs) takes about 23 kB of DRAM before
`setup()` runs: 220 kB free at boot against 244 kB with the USB-Serial-JTAG
console. WiFi off while the app is on the link (later in 1.6) gives back about
84 kB.
