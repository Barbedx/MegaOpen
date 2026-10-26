# MegaOpen Companion

The board's app on the Android head unit (package `ua.megaopen.companion`): the
panel alerts out loud, the board's page, the board's way to the internet. Over
the USB link when the board is plugged in, over WiFi otherwise.

Design, layers and configuration: `docs/design/companion-app.md`; the link:
`docs/design/usb-link.md`.

Build: JDK 17, Gradle 8.9 (no wrapper jar), an Android SDK with platform 34.

    gradle -p android testDebugUnitTest lintRelease assembleRelease

CI runs the same (`ci.yml` job `android`; `release.yml` signs with the release
key from the repo secrets and publishes `megaopen-companion-X.Y.Z.apk`). The
version is `MEGAOPEN_VERSION` from `src/Version.h`.

Signing: release.yml decodes `ANDROID_KEYSTORE_B64` (PKCS#12, alias `megaopen`,
password `ANDROID_KEYSTORE_PASS`) and checks the APK's certificate (SHA-256
`c6dd740b…f8c6ae`). Lose the key and every install needs an uninstall once; the
owner keeps a copy off GitHub.
