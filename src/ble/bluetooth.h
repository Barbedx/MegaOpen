#pragma once
#include <string>
#include <Arduino.h>

// The AMS role: our GATT client onto the iPhone over the connection the phone
// made to us (we are GAP peripheral; phone is GATT server for AMS/ANCS/CTS).
// BleHub owns the stack and drives this. Never call NimBLEServer::getClient()
// here; use BleHub::bindAmsClient() (one-client invariant).
namespace Bluetooth {

// --- driven by BleHub, on ble_svc ---
// Requests from other tasks (OnAmsPeerLost, ClearBonds), performed here.
// Every pass, whatever the roles.
void ServicePending();
void ServiceAms();     // per-loop AMS setup/keepalive; called by BleHub::Service()

// Any task. The peer holding our client went away: ble_svc drops every cache
// (AMS, ANCS, CTS, the client) at its next pass. Only ble_svc drops them, and
// only ble_svc rebinds the client (whose getClient() frees the old GATT
// tree), so a cached characteristic is never freed or nulled under a write.
void OnAmsPeerLost();

// --- public status ---
bool IsConnected();
// ble_svc, before BleHub::Service(): key work is owed (a HID release, AMS
// key writes), so ServiceAms() leaves its slow GATT reads for a later pass.
void SetKeysBusy(bool busy);
// The AMS link's connection interval in ms (30 without a link). ble_svc.
uint16_t ConnIntervalMs();
bool IsTimeSet();
bool HasBond();
// Any task: forget every bond; ble_svc does it at its next pass.
void ClearBonds();

const char* GetStatusText(); // short status for the car display
String      GetStatusJson(); // legacy keys + nested "ams"/"hid" objects

} // namespace Bluetooth
