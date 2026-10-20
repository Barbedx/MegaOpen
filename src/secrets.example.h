#pragma once
// Build-time secrets. Copy this file to src/secrets.h (gitignored) and fill it
// in; without src/secrets.h the build uses the values below.
//
// They are DEFAULTS: what a fresh or factory-reset board starts with. Anything
// saved from the web app (NVS) wins over them.

// The board's own access point. 8 to 63 characters, or the AP comes up open.
#define MO_AP_PASS   "megaopen"

// A WiFi network to join when none is saved, so a factory reset does not lose
// the car's network. Empty = start as an access point.
#define MO_WIFI_SSID ""
#define MO_WIFI_PASS ""
