# MegaOpen Companion (the head unit's app)

`android/`, package `ua.megaopen.companion`. The board's app on the Android head
unit: the panel alerts out loud, the board's own page, the board's way to the
internet (updates, log push), later a dashboard. It talks to the board over the
USB link (docs/design/usb-link.md) when the board is plugged in, over WiFi
otherwise, and the rest of the app does not care which.

Owner's call, 2026-10-26: a product for other people's cars, not a prototype.
No root, no hard-coded addresses, every word in resources (uk, en), tests in CI.

## Layers

| package | what | depends on |
| --- | --- | --- |
| `link` | frames (`Frames.kt`, the board's `LinkFrame`), HTTP on the wire (`HttpWire.kt`), the USB transport (`UsbLink.kt`: device, permission, reader, channels, events, the board's internet) | Android USB |
| `board` | `Board`: one interface over both ways in (`UsbBoard`, `WifiBoard`), `Discovery` (mDNS, the hotspot's /24), `BoardConnection` (which one now, its state as a `StateFlow`) | `link` |
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

Coroutines. The USB reader is one blocking loop on `Dispatchers.IO`; requests
are `suspend` and wait on a `CompletableDeferred` per channel; the pinger and the
WiFi poll are coroutines in the service's scope. The UI reads `StateFlow`s.

## Tests

`app/src/test`: the frames (the same cases as `test/test_link`), the HTTP
parser (Content-Length, chunked, gzip, cut), the version order, the phrases.
CI runs `gradle test lint assembleRelease`; lint errors fail the build.
