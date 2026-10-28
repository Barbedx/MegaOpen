# Companion over USB: now playing, keys anywhere, a virtual panel

Status: **plan**, 2026-10-28, owner's ask: "take as much information as we can
and make a great media screen, maybe with the cover as a bitmap; use USB HID
instead of the UART for keys and map them where we want; a «virtual keyboard»
page to check every key." Builds on the USB link (docs/design/usb-link.md) and
MegaOpen Companion (docs/design/companion-app.md). Not started.

## Today

| what | how | limit |
| --- | --- | --- |
| now playing on the panel | `HuMediaSource`: the head unit's UART text frame `0x99` (short fields, its own charset, only what DUDU's own player reports); `AmsMediaSource`: the iPhone over BLE | no app name, no cover, nothing from Spotify/YouTube Music on the head unit unless DUDU forwards it |
| panel keys to the head unit | `KeyRouter` → destinations `uart` (RAV4 key codes), `ams` (iPhone), `hid` (env `t2can_usbhid` only: media keys) | UART codes are what the canbox protocol defines; HID is a separate image without WebUSB |
| checking keys | the panel itself, `/api/display/key` (one panel key from the page) | no page that shows every key and where each one went |

`MediaInfo` (src/media/MediaInfo.h) already holds what AMS gives: title,
artist, album, player name, state, rate, elapsed, duration, queue, shuffle,
repeat, volume. The panel has a 48×48 one-bit picture pane (the owner's own
picture, the tryzub scenes).

## 1. Now playing from the head unit's Android

Companion reads the **active media session** of whatever plays on the head
unit (Spotify, YouTube Music, a radio app, DUDU's player if it publishes one):
`MediaSessionManager.getActiveSessions` through a `NotificationListenerService`
(the user grants «Доступ до сповіщень» once; the app shows how).

- **What it sends** (link type `B0 MEDIA`, app → board, JSON): `title`,
  `artist`, `album`, `app` (the player's label), `state`, `pos`/`dur` (ms) with
  the time they were read, `queue` when the session has one. On every metadata
  or state change, and on HELLO; never polled.
- **The cover**: `B1 ART`, app → board: Companion scales the session's
  `METADATA_KEY_ART` to 48×48, grey, Floyd–Steinberg to one bit: 288 bytes plus
  a 4-byte hash. The board asks only for a hash it does not have (a cover is
  sent once per track, not per state change).
- **On the board**: `AppMediaSource`, a third `IMediaSource` beside HU and AMS,
  filled from the link task. `MediaRouter` picks among the three
  (source priority on «Екран»: app > HU > iPhone by default, while each is live).
- **The media screen**: the header is the player (`app`); title and artist
  scroll on the text row; a progress line from pos/dur (extrapolated, as AMS
  does); the cover on the 48×48 pane as one of the media scenes (carousel stop
  only while it has one). The panel's limits stay the limits: 8 characters on
  the text row, 22 on the fullscreen layout.

## 2. Keys: to the app, and as USB HID, mapped anywhere

Two new destinations in `KeyRouter`, mapped per key like `uart`/`ams` today:

- **`app`**: the key goes over the link (`A1 KEY`, board → app: key, press/
  release/hold, the knob detents). Companion maps it to an **action**: the
  active session's transport controls (play/pause/next/previous/seek, which
  reach Spotify directly, not through a keycode), volume, open an app,
  voice assistant, navigation home, or a custom intent. The map lives in the
  app (its settings screen), with factory rows.
- **`hid`** in the main image: the USB composite gets the HID interface next to
  CDC and the link, so the separate `t2can_usbhid` env goes. Consumer page
  (media, volume, AC Back/Home) and keyboard page (arrows, Enter, Escape) per
  key; Android handles them system-wide, with no app running. WebUSB today needs
  the vendor interface at 0 (usb-link.md: on `t2can_usbhid` interface 0 is the
  HID, so WebUSB stays off): slice 2 starts by checking a descriptor order that
  keeps both, and if none does, the owner picks (HID or the WebUSB page).

The UART codes stay for what only DUDU's own screens read (com.syu.ms). Per
key the owner chooses any mix: uart, app, hid, ams.

## 3. «Віртуальна панель» page

A page (Кнопки › «Віртуальна панель», `#vpanel`) drawn like the real panel:
the front keys, the steering-wheel remote, both knobs (turn left/right, click,
hold). Pressing one goes into `KeyRouter` as the panel's own key would (a
`Web` lane source, as `/api/display/key` does), so every destination answers
as in the car. Each press shows a line: key → which destinations took it
(uart code sent, app action, HID usage), with latency. Real panel presses light
up on it too (live, from the `[key]` path), so the page also checks the panel's
wiring. In the simulator the mock answers the same.

## Slices

1. **Media from the app**: Companion's listener + `B0 MEDIA`; `AppMediaSource`,
   `MediaRouter` priority, `/api/media` shows the source; the panel text. Host
   tests for the frame and the source; the app's unit tests.
2. **The cover**: `B1 ART`, the dither in the app (tested on fixed images), the
   pane scene.
3. **Virtual panel page** (no protocol change; useful before 4 and 5).
4. **Keys to the app**: `A1 KEY`, the `app` destination, Companion's action map.
5. **HID in the main image**: the composite descriptor, the `hid` destination
   for every key, `t2can_usbhid` deleted; the WebUSB decision.
6. **Docs**: USER-GUIDE (the permission, the screen, the key map), CHANGELOG.

## Depends on

- Rules 1.7 (session megaopen-03) owns `HuMediaSource`, `HuTask`,
  `src/proto/canbox` until its step 5b lands: slice 1 touches `MediaRouter`
  only, after that.
- The head unit: Android version and whether DUDU's own player publishes a
  media session (one check on the car with the app's listener on).

## Open questions (owner)

1. Source priority when both play: the app over the iPhone, always?
2. The cover as a scene: only while playing, or also paused?
3. HID vs the WebUSB page, if one descriptor cannot have both.
4. The app's action list for keys: what else beyond media, volume, open an
   app, voice, navigation?
