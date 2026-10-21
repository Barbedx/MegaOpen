// Whole-body gate, like BleKeySinks.cpp: env:direct compiles every file in
// sinks/ and has no NimBLE.
#include "BleKeyProxy.h"
#include "../keys/KeyDests.h"
#if FEATURE_BLE
#include <Arduino.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace Sinks
{
    namespace BleKeyQueue
    {
        namespace
        {
            QueueHandle_t         s_q = nullptr;
            std::atomic<uint32_t> s_dropped{0};
        }

        bool begin()
        {
            if (!s_q) s_q = xQueueCreate(DEPTH, sizeof(BleCmd));
            return s_q != nullptr;
        }

        bool post(const BleCmd& c)
        {
            if (s_q && xQueueSend(s_q, &c, 0) == pdTRUE) return true;
            s_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        bool take(BleCmd& out, uint32_t waitMs)
        {
            if (!s_q) { vTaskDelay(pdMS_TO_TICKS(waitMs ? waitMs : 1)); return false; }
            return xQueueReceive(s_q, &out, pdMS_TO_TICKS(waitMs)) == pdTRUE;
        }

        uint32_t dropped() { return s_dropped.load(std::memory_order_relaxed); }
    }

    void BleKeyProxy::onKey(const Keys::Event& e)
    {
        // Both are one-shot commands; acting on release would fire twice.
        if (e.state != Keys::State::Press && e.state != Keys::State::Repeat) return;
        const uint8_t c = Keys::classOf(e.action);
        const uint8_t dest = _target == BleCmd::Target::Ams ? Keys::DEST_AMS : Keys::DEST_HID;
        if (c == Keys::CLASS_NONE || (_mask & Keys::DEST_CLASSES[dest] & c) == 0) return;
        if (!Keys::destActs(dest, e.action)) return;

        BleCmd cmd;
        cmd.target = _target;
        cmd.action = e.action;
        cmd.atMs   = millis();
        if (BleKeyQueue::post(cmd)) ++_posted;
    }
}
#endif
