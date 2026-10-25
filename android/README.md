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

Built by CI only (`ci.yml` job `android`, `release.yml` for the release asset):
JDK 17, Gradle 8.9 (no wrapper jar), the runner's Android SDK. Locally, with an
SDK: `gradle -p android assembleRelease`. The version is `MEGAOPEN_VERSION`
from `src/Version.h`.
