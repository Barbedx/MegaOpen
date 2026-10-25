# MegaOpen Voice

The head unit's voice for the board's panel alerts. A foreground service polls
`GET /api/alerts?since=N` (src/net/WebUiMap.cpp) and says each new alert with
the device's text-to-speech, ducking the music. Settings are one screen
(`MainActivity.kt`, built in code); the factory alerts and their phrases are in
`Phrases.kt`.

| file | what |
| --- | --- |
| `Board.kt` | the HTTP calls, mDNS (`_megaopen._tcp`) and the /24 scan |
| `AlertService.kt` | the poll loop: `since`, reboot (`upMs` down), stale alerts |
| `Speaker.kt` | TTS, language, volume, audio focus |
| `BootReceiver.kt` | start with the head unit |
| `Updater.kt`, `InstallReceiver.kt` | self-update from GitHub releases (PackageInstaller, SHA256SUMS) |

Built by CI only (`ci.yml` job `android`, `release.yml` for the release asset):
JDK 17, Gradle 8.9 (no wrapper jar), the runner's Android SDK. Locally, with an
SDK: `gradle -p android assembleRelease`. The version is `MEGAOPEN_VERSION`
from `src/Version.h`.

Signing: release.yml decodes the secret `ANDROID_KEYSTORE_B64` (PKCS#12, alias
`megaopen`, password `ANDROID_KEYSTORE_PASS`) into `MO_KEYSTORE` and checks the
APK's certificate (SHA-256 `c6dd740b…f8c6ae`). Lose the key and every install
needs an uninstall once; the owner keeps a copy off GitHub.
