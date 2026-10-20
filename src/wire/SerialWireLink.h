#pragma once
#include <Arduino.h>
#include "WireLink.h"

// UART link: every LOGI/LOGW/LOGE goes to Serial, so logs can be read with WiFi
// idle. Unused since the LogBus (main.cpp's BusWireLink) carries every line.
//
// Cost: Serial.println blocks the logging task until the UART takes the bytes,
// and the AFFA protocol is timing-sensitive. If `[disp] REGISTRATION LOST` or
// the resyncs counter in /api/health climb, suspect this first.
struct SerialWireLink : WireLink {
    void emitLine(const char* line) override { Serial.println(line); }
    void onCommand(CommandCb cb, void* ctx) override { (void)cb; (void)ctx; }
    bool connected() const override { return true; }
};
