#pragma once
#include <string>
#include <Arduino.h>

// The AMS role: our GATT client onto the iPhone over the connection the phone
// made to us (we are GAP peripheral; phone is GATT server for AMS/ANCS/CTS).
// BleHub owns the stack and drives this. Never call NimBLEServer::getClient()
// here; use BleHub::bindAmsClient() (one-client invariant).
namespace Bluetooth {

// --- driven by BleHub ---
void ServiceAms();     // per-loop AMS setup/keepalive; called by BleHub::Service()
void OnAmsPeerLost();  // the peer holding our client went away — drop all caches

// --- public status ---
bool IsConnected();
bool IsTimeSet();
bool HasBond();
void ClearBonds();

const char* GetStatusText(); // short status for the car display
String      GetStatusJson(); // legacy keys + nested "ams"/"hid" objects

} // namespace Bluetooth
