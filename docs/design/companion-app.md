# MegaOpen Companion (the head unit's app)

`android/`, package `ua.megaopen.companion`. The board's app on the Android head
unit: the panel alerts out loud, the board's own page, the board's way to the
internet (updates, log push), what plays on the head unit for the panel, later
a dashboard. It talks to the board over the USB link (docs/design/usb-link.md)
when the board is plugged in, else over the same link on Bluetooth LE, else
over WiFi, and the rest of the app does not care which.

Owner's call, 2026-10-26: a product for other people's cars, not a prototype.
No root, no hard-coded addresses, every word in resources (uk, en), tests in CI.

## Layers

| package | what | depends on |
| --- | --- | --- |
| `link` | frames (`Frames.kt`, the board's `LinkFrame`), HTTP on the wire (`HttpWire.kt`), the session (`LinkSession.kt`: HELLO, ping, channels, events, the board's internet) over a `Pipe`; the transports: `UsbLink.kt` (device, permission, bulk endpoints), `BleLink.kt` (GATT, pairing, reconnect, the picker's scan), `BleBoard.kt` (the UUIDs, the name match, the backoff) | Android USB, Bluetooth |
| `board` | `Board`: one interface over every way in (`LinkBoard` on USB or Bluetooth, `WifiBoard`), `Discovery` (mDNS, the hotspot's /24), `BoardConnection` (which one now, its state as a `StateFlow`) | `link` |
| `media` | `MediaWatch` (the head unit's media sessions, through `MediaListener`'s notification access), `Cover` (48×48 one-bit cover, pure), `NowPlaying` (the post's JSON, pure), `MediaSender` (to the board) | `board` |
| `alerts` | the alert model, the factory alerts and their phrases, what to say for an alert | — |
| `voice` | `Speaker`: TextToSpeech, language, volume, audio focus (the music ducks) | — |
| `update` | `Releases` (GitHub's list, the version order, an rc channel), `Installer` (download, SHA256SUMS, PackageInstaller) | — |
| `settings` | `Settings`: every setting as a typed `Flow`, in DataStore | — |
| `service` | `CompanionService`: the foreground service (type `connectedDevice`) that owns the connection and the voice; boot and install receivers | all above |
| `ui` | Compose screens: Home, Voice, Board (the board's page in a WebView), Updates, Diagnostics | `service` state |

`AppGraph` (in `App.kt`) builds the singletons once; nothing else creates them.

## Configuration

Build-time, in `app/build.gradle.kts` as `BuildConfig` fields: the GitHub repo
releases come from (`RELEASES_REPO`), the hosts the app goes to for the board
(`NET_HOSTS`, the board's `GhRelease::trustedUrl` list), the board's USB VID/PID.
A fork changes them there, in one place. The version is `MEGAOPEN_VERSION`
(`src/Version.h`): one release, one number.

## The ways to the board

More › «Підключення» picks them: Авто (USB, then Bluetooth, then WiFi), Лише
USB, Лише Bluetooth, Лише WiFi. `BoardConnection` walks them in that order
each round; a better way coming up cuts a pause short.

USB and Bluetooth carry the same frames (docs/design/usb-link.md). One
`LinkSession` holds the conversation; under it a `Pipe` moves bytes:

| | USB (`UsbLink`) | Bluetooth LE (`BleLink`) |
| --- | --- | --- |
| bytes out | bulk OUT, whole frames | RX characteristic `4d4f0101-…`, write with response, MTU-3 a write, one at a time |
| bytes in | bulk IN, a reader thread | TX characteristic `4d4f0102-…`, notifications |
| access | Android's USB permission dialog | BLUETOOTH_CONNECT (12+); the first write pairs (Just Works) |
| finding | VID/PID and a vendor interface | chosen once in a picker, then by address |

Service `4d4f0100-6c69-6e6b-8000-4d6567614f70`. After the connection: MTU
247, the services, notifications on (CCCD), then HELLO. A write the board
refuses for want of encryption waits for the bond and goes again. The picker
lists paired devices and a 15 s scan: the advertising name "MEGA", a scan
response with "MegaOpen", or the service UUID (the board's link-only mode),
or the name the app met before. Not found: on the board, Плата › Bluetooth
must be any mode but «Вимкнено».

Reconnects: the first try a direct connection (15 s), then Android's
background connection (autoConnect) for a 30 s window; between tries a pause
of 2 s doubling to 30 s, back to 2 s after a connection that came up. Home and
Diagnostics show each allowed way's state and the seconds to its next try;
WiFi's search and its lost board too.

## Now playing

`MediaWatch` follows Android's active media sessions (Spotify, YouTube Music,
radio apps, the head unit's player if it publishes one). Android lets an app
read them while it holds a notification listener the user switched on:
`MediaListener` exists for that and reads no notification. Home › «Медіа» says
whether the access is given and opens the system screen for it.

The session that plays wins, else the one that played last. `MediaSender`
posts `/api/app/media` on a change (300 ms quiet first; a position the player
merely reports again is no change, a seek is), on every (re)connection and
every 20 s while a session lasts; `"state":"none"` once when it is gone. The
board answers `needArt` when it lacks the cover's hash; then `/api/app/art`
sends the 288 bytes as hex. A refusal is plain text, a code and maybe an
argument (`param state`, `art_stale`); it shows on the card and in the log,
and the next change or keepalive tries again (`art_stale` re-posts the media
once at once).

The cover (`Cover.of`): centre-crop to a square, area average to 48×48,
luma, 2nd–98th percentile stretched, Floyd–Steinberg to one bit, 1 = lit;
row-major, MSB first; hash = CRC-32 of the 288 bytes, 8 lowercase hex. The
card draws exactly those bits.

## The board's page

The app carries the board's settings page: the same web/ files the board
serves, exported at build time (`tools/export_page.py` over `tools/gen_pages.py`)
into `assets/page/` with the version they were built for. `ui/BoardScreen.kt`
shows the app's copy when the board runs that same firmware, or when the board
has no page (`FEATURE_WEB_PAGE=0`: "/" is the rescue page, marked
`X-MegaOpen-Page: rescue`, and `/i18n/en.json` a 404); else the
board's own page, which knows the board's own API. The page's API calls always
go to the board, through `Board`, over USB or WiFi alike.

## Threads

Coroutines. The USB reader is one blocking thread, BLE notifications come on
the GATT callback thread, both feed the session's parser; writes are one at a
time under the session's lock (BLE: each chunk waits for its write callback);
requests
are `suspend` and wait on a `CompletableDeferred` per channel; the pinger and the
WiFi poll are coroutines in the service's scope. The UI reads `StateFlow`s.

## Tests

`app/src/test`: the frames (the same cases as `test/test_link`), the HTTP
parser (Content-Length, chunked, gzip, cut), the session over a fake board
whose bytes come back cut anywhere (HELLO, requests, events, ping), the BLE
name match and backoff, the cover (white, black, a gradient, a checker, the
crop, the stretch, the bit layout and hash), the media post's JSON and
position, the version order, the phrases.
CI runs `gradle test lint assembleRelease`; lint errors fail the build.
