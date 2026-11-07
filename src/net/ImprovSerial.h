#pragma once
#include <stdint.h>

class WifiManager;

// Improv Wi-Fi on the USB serial (the protocol: src/net/Improv.h). The web
// installer's «Configure Wi-Fi» after a flash, or «Встановити» on a running
// board: it lists the networks the board sees and hands it a password, and the
// board joins at once and answers with its address.
//
// net_svc only: tick() runs right after WifiManager::loop() (main.cpp hkWifi),
// so provisioning calls WifiManager on its own task, no lock. It reads Serial,
// which nothing else reads; the log keeps writing to it from every task, and
// the client skips those lines.
namespace ImprovSerial {
void begin(WifiManager& wifi);
void tick(uint32_t nowMs);
}
