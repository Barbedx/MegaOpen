# The board's AP password (security review P1)

**Held: not merged without the owner.** It changes how you reach the car's
access point.

## What changes

* A fresh or factory-reset board makes up its own AP password: 10 letters
  and digits, without look-alikes (no 0/o, 1/l/i). It shows it on the car's
  display at that boot: a fullscreen `WiFi MegaOpen-XXXX` / `pass …`.
* The panel menu gets a root row **WiFi pass**, just above Power off. It
  shows the current password on the glass. Being in the car is the proof;
  the web and the web log never show it.
* A board that still has the public default `megaopen` keeps using it, so no
  one is locked out. It makes up a candidate and shows `new pass …: menu,
  WiFi pass` at every boot. Choosing **WiFi pass** then switches to the
  candidate from the next start. Setting your own password on Плата also ends
  it.
* The web refuses `megaopen` as a new password.
* A dev build can preset one in `src/secrets.h` (`MO_AP_PASS`). A build
  without a panel (`direct`) keeps the old behaviour, because it has nowhere
  to show a made-up one.

Code: `src/cfg/ApPass.h` (pure, host-tested), `Config::loadApPass`,
`adoptApPass`, `setOwnApPass`, the menu row (`MenuEngine` `Kind::WifiPass`,
`PanelMenu`), and the boot notice in `main.cpp`.

## What the owner must do

After flashing this build, start the car and watch the display. At this
boot, and every boot until you act, it shows `new pass xxxxxxxxxx: menu,
WiFi pass`, while the AP still answers to `megaopen`. When you are ready,
hold the right knob, choose **WiFi pass**, and note the password it shows.
From the next start the AP `MegaOpen-XXXX` takes only that password, so
forget the old network on your phone and rejoin. Alternatively, set a
password of your own on Плата → WiFi (8+ characters, not `megaopen`); that
also ends the prompt. If you ever lose it: the **WiFi pass** row shows it
again in the car. A factory reset makes a new one and shows it once.

## Open questions for the owner

* Should the old default stop working on its own after N boots, instead of
  waiting for the menu? Today it never forces.
* Is the fullscreen at every boot (6 s) acceptable until you act, or should
  it be once a day?
