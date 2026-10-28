# Releasing

Two tags, one set of bytes: an rc is built and tried on a board, and the
release republishes that rc's exact files. The workflow is
`.github/workflows/release.yml`; its header is the reference.

## Steps

1. **Version.** In `src/Version.h` set `MEGAOPEN_VERSION` to `X.Y.Z` and
   `MEGAOPEN_VERSION_WIRE` to `"MegaOpen X.Y.Z"` space-padded to 16 bytes.
   Add a `## [X.Y.Z]` section to `CHANGELOG.md` (the release notes come from
   it). Commit, push to `master`.
2. **CI green** on that commit (`.github/workflows/ci.yml`).
3. **Tag the rc** on that commit and push that one tag:
   `git tag vX.Y.Z-rc1 && git push origin vX.Y.Z-rc1`. The workflow checks
   the version and the CHANGELOG section, builds every env, refuses a
   `-dirty` build stamp, and publishes a **pre-release** with
   `megaopen-<env>-X.Y.Z.bin`, `-factory.bin` and `SHA256SUMS`.
4. **OTA-verify** the rc's `megaopen-t2can-X.Y.Z.bin` on a real T-2CAN
   (Плата → Оновлення прошивки, the file) and wait for `[ota] CONFIRMED` in
   the log (60 s healthy). Then `tools/rc_smoke.sh http://<board>` on it:
   every route, the deep ones (the settings file, the map) included, and no
   reboot during the run. CONFIRMED alone is not enough: 1.6.3-rc1 was
   confirmed and its `GET /api/config` crashed the web server. A problem:
   fix, commit, `-rc2`.
5. **Tag the release** `vX.Y.Z` on the **same commit**, only after the rc's
   release exists: `git tag vX.Y.Z <commit> && git push origin vX.Y.Z`. It
   does not rebuild: it takes the newest `vX.Y.Z-rc*` release on that commit,
   checks `SHA256SUMS` and publishes those files as **Latest**. No rc on the
   commit, no release. Boards updating "from GitHub" take Latest.

## AffaDisplay first

The library is pinned by tag in `platformio.ini` (`#vA.B.C`). A library
change is released there first (its own tag), then MegaOpen bumps the pin in
a commit of its own, then the steps above.

## Rules

* Push tags one by one by name. Never `git push --tags`: old local tags
  live under `refs/archive/tags/`, and a bulk push would publish them.
* Commits and tags use the repo-local identity (`www.andruxa@gmail.com`);
  check `git log -1 --format='%ae %ce'` before pushing.
* A release image never carries a `-dirty` stamp: build from a clean tree
  (`tools/version.py` ignores generated files).
* Never force-push `master`, never move a published tag.
