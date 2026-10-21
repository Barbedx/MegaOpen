# The board's AP password (security review P1)

**Held: not merged without the owner.** It changes how you reach the car's
access point.

## What changes

* A fresh or factory-reset board makes up its own AP password: 12 letters
  and digits without look-alikes (no 0/o, 1/l/i), drawn with the chip's
  entropy source on. Until you acknowledge it, it is shown at **every** boot:
  on the car's display (`WiFi MegaOpen-XXXX` / `pass …: menu, WiFi pass`)
  and on the USB console.
* The panel menu gets a root row **WiFi pass**, just above Power off. It
  shows the current password on the glass and acknowledges it (no more
  notice at boot). No web route shows it. While it is on the glass, the
  panel frames that carry it are kept out of the trace and the raw frame
  ring, which the web can read.
* A board that still has the public default `megaopen` keeps using it, so no
  one is locked out. It makes up a candidate and shows `new pass …: menu,
  WiFi pass` at every boot (display and USB). Choosing **WiFi pass** switches
  to the candidate from the next start. Setting your own password on Плата
  also ends it.
* The web refuses `megaopen` as a new password.
* **Recovery:** hold the board's **BOOT** button while it starts its WiFi
  (press it just after power-up, not before: BOOT held at the very moment of
  power-up is the chip's flashing mode). That start, the AP takes the public
  default `megaopen`; the display says `recovery`, the log warns, and nothing
  is saved. The next normal start uses the board's password again.
* A dev build can preset one in `src/secrets.h` (`MO_AP_PASS`). A build
  without a panel (`direct`) keeps the old behaviour, because it has nowhere
  to show a made-up one.

Code: `src/cfg/ApPass.h` (pure, host-tested), `Config::loadApPass`,
`adoptApPass`, `ackApPass`, `setOwnApPass`, `apPassForAp`; the menu row
(`MenuEngine` `Kind::WifiPass`, `PanelMenu`); the boot notice, the BOOT
recovery and the trace mute in `main.cpp` / `UiTask` (`queueSecretNotification`).

## Cars without knobs

The menu opens on `Action::Menu`, by default the right knob's hold. Without
knobs, map a steering-wheel key to it: **Кнопки → Кермо → дія**, e.g. the
Pause key's hold row → Меню. The panel's own Load/Pause gestures are all taken
by default (Load and Pause, each with a long press), so none is reassigned;
choose the one you can spare.

## What the owner must do

After flashing this build, start the car and watch the display. At this
boot, and every boot until you act, it shows `new pass xxxxxxxxxxxx: menu,
WiFi pass`, while the AP still answers to `megaopen`. When you are ready,
hold the right knob, choose **WiFi pass**, and note the password it shows.
From the next start the AP `MegaOpen-XXXX` takes only that password, so
forget the old network on your phone and rejoin. Alternatively, set a
password of your own on Плата → WiFi (8+ characters, not `megaopen`); that
also ends the prompt. Lost it: the **WiFi pass** row shows it again in the
car, the USB console prints it while unacknowledged, and the BOOT recovery
gets you in with `megaopen` for one start. A factory reset makes a new one
and shows it at every boot until you press **WiFi pass**.

## Open questions for the owner

* Should the old default stop working on its own after N boots, instead of
  waiting for the menu? Today it never forces.
* Is the 6 s notice at every boot acceptable until you act?
