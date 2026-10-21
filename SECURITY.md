# Security

MegaOpen is a hobby firmware wired into a car. This page says what it
exposes and how to report a problem.

## What is exposed

* **WiFi.** With no home network in range (in the car) the board runs its own
  access point, `MegaOpen-XXXX`. Its default password is `megaopen`, which is
  public. **Change it at first boot** (Плата page). At home the board joins
  your WiFi and is reachable by anything on that network.
* **No login.** The web app, the API and OTA have no password of their own:
  whoever is on the board's network can change settings, reset it and flash
  firmware. The WiFi/AP password is the only lock.
* **Firmware.** An image flashed over WiFi is not signature-checked. Our
  firmware keeps the car's CAN bus (CAN A) listen-only, but that is enforced
  in software: a foreign image could transmit on it.
* **Other web sites.** The board answers only to its IP or its own `.local`
  name, and every change needs the header `X-MegaOpen: 1`, which a page on
  another site cannot send. So a page you open while on the board's network
  cannot reboot, reset or flash it. Tools: `curl -H "X-MegaOpen: 1" ...`.
  Every GET is side-effect-free except the two that also need the header
  (`/ota/start`, `/api/cap/mark`); a WiFi scan is a POST.
* **Not exposed:** no route returns a WiFi or AP password; the settings file
  never contains them.

Work in progress (docs/BACKLOG.md): a per-board AP password shown only on
the car's display, a physical "allow update" step, signed images, and a hardware mod that makes CAN A
receive-only (the TX line broken, docs/BACKLOG.md "Hardware").

## Reporting

Please do not open a public issue with the details. Use GitHub's private
**Report a vulnerability** (the repository's Security tab). If that is not
available, open an issue asking for a contact, without details.
