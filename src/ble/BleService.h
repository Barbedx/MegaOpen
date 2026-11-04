#pragma once
#include "../Features.h"
#if FEATURE_BLE
#include <Arduino.h>
#include "../cfg/Settings.h"
#include "../sinks/BleKeySinks.h"

// The phone side at boot and its task: the BLE key queue, the mode from the
// settings (AMS, keyboard, both or off), the AMS media source, ANCS, and
// ble_svc, which services the stack and performs the keys' AMS writes and HID
// presses (sinks/BleKeySinks.h).
namespace BleService
{
    // setup(), after the key sinks exist and before the keys task starts.
    void begin(const Cfg::Settings& s);
    // ble_svc, once it runs (BleHub::Begin finishes on its own task); null before.
    TaskHandle_t task();
    // The keys' BLE path counters, for the [ble] line.
    const Sinks::BleKeyWorker& keys();
#if FEATURE_ANCS
    // Notifications received since boot.
    const uint32_t* ancsCount();
#endif
}
#endif
