#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Every task's priority, core and stack in one table (docs/design/event-tasks.md
// §2.1). Read this before adding a task or moving one: who preempts whom is
// what decides key latency, and it used to be scattered over six files.
//
// The table says what runs TODAY. Where the plan moves a task in a later step,
// the comment says which step and to what, so the two cannot be confused.
//
// Stacks are per chip. The DevKit (classic ESP32, env:direct) values are the
// tight ones: its idle free heap is ~80 kB. The S3 has room to spare.
namespace App
{
    // Core n, or core 0 on a single-core chip; both boards built today are
    // dual-core. A negative n means "either core" (tskNO_AFFINITY).
    constexpr BaseType_t core(int n)
    {
        return n < 0 ? (BaseType_t)tskNO_AFFINITY
                     : (portNUM_PROCESSORS == 1 ? 0 : (BaseType_t)n);
    }

#if CONFIG_IDF_TARGET_ESP32S3
    constexpr bool BIG_STACKS = true;
#else
    constexpr bool BIG_STACKS = false;
#endif
    constexpr uint32_t stack(uint32_t s3, uint32_t tight) { return BIG_STACKS ? s3 : tight; }

    namespace Plan
    {
        // ---- running now --------------------------------------------------------
        // The knobs sample at 1 kHz and must not starve: a late sample drops a
        // detent (KnobService.cpp). 6 puts them above the loop, which owns the
        // panel and spends whole milliseconds in a render. Their work is ~0.2 ms
        // of I2C per 1 ms tick, which the loop can afford to lose.
        constexpr UBaseType_t KNOBS_PRIO = 6;
        constexpr int         KNOBS_CORE = 1;
        // 2048 measured 0.8 kB used with both rings lit and turning; the I2C
        // driver call of the PCF8574 boards adds ~1 kB on this stack, and the
        // LogBus::printf lines on expander lost/found/stuck add a 320 B buffer
        // plus vsnprintf (unmeasured with the cable out).
        constexpr uint32_t    KNOBS_STACK_GPIO = 2048;
        constexpr uint32_t    KNOBS_STACK_I2C  = stack(4096, 3072);

        // Matrix builds only (env:direct). A 1 kHz sweep that only posts its
        // edges to the KeyBus: beside the knobs, for the same reason.
        constexpr UBaseType_t SCAN_PRIO  = 6;
        constexpr int         SCAN_CORE  = 1;
        constexpr uint32_t    SCAN_STACK = 4096;

        // The one key dispatcher (src/app/KeysTask.h): above everything that
        // renders, so a key never waits for a repaint. It wakes on a post or
        // on the exact next sink deadline; its passes are microseconds.
        constexpr UBaseType_t KEYS_PRIO = 7;  constexpr int KEYS_CORE = 1;
        constexpr uint32_t    KEYS_STACK = stack(4096, 3072);

        // The head-unit link (src/app/HuTask.h): Serial1 RX and the ack, and
        // every non-key frame. Below the keys, above the panel: an ack late
        // by more than 10 ms is resent by the head unit. Core 1 with the keys
        // task, because only core-1 tasks take the UART mutex (§2.1).
        // 3072 left only ~840-1060 bytes free on the S3 at idle ([task] hu
        // stk=), with the 17-argument log lines and the frame callbacks still
        // to come under load: 4096 on the S3 and the DevKit (+1 kB of heap; the
        // DevKit idles near 80 kB).
        constexpr uint32_t    HU_STACK   = 4096;
        constexpr UBaseType_t HU_PRIO   = 6;  constexpr int HU_CORE   = 1;

        // The Arduino loop task is the panel task (src/app/PanelOwner.h): the
        // only caller of AffaDisplay and of the TWAI driver. Since Step 4 it
        // only pumps; the render policy is `ui`'s. 5: below the keys, the hu
        // task and the 1 kHz samplers, above everything that only renders or
        // talks to the network. It sleeps in twai_receive(), so a panel frame
        // wakes it; its stack is Arduino's own 8 kB.
        constexpr UBaseType_t LOOP_PRIO = 5;

        // The render policy (src/app/UiTask.h): what goes on the glass and
        // when. It only posts to the mailbox, so it can sit at 2 on the key
        // core, below everything that a key or the panel's timing waits for;
        // a slow compose or the 5 ms media-lock wait costs a late repaint and
        // nothing else. Wakes on a completion, or every 50 ms.
        // 6 kB on the S3. The 4 kB for a classic ESP32 (no build runs the ui
        // task there today: env:direct has no panel) is why the compose and
        // fold buffers are static, not on this stack. Read its [task] low-water.
        constexpr UBaseType_t UI_PRIO   = 2;  constexpr int UI_CORE   = 1;
        constexpr uint32_t    UI_STACK   = stack(6144, 4096);

        // net_svc, the housekeeping task (src/app/Housekeeping.h): the web
        // servicing, WiFi, the heartbeat and the OTA
        // verdict. On the WiFi core and below everything; 6144 because the
        // heartbeat's lines and the WiFi/String calls now run on it.
        constexpr UBaseType_t NET_PRIO  = 1;
        constexpr int         NET_CORE  = 0;
        constexpr uint32_t    NET_STACK = 6144;

        // BleHub::Service (AMS, ANCS) and the BLE key commands: AMS writes and
        // HID presses posted by the keys task. 2 on core 0, with the radio and
        // NimBLE, and off the key core: an AMS write blocks for up to 130 ms.
        // 4096 measured under 2 kB with AMS and ANCS live.
        constexpr UBaseType_t BLE_PRIO  = 2;
        constexpr int         BLE_CORE  = 0;
        constexpr uint32_t    BLE_STACK = 4096;

        // LogBus: below everything, on the WiFi core.
        constexpr UBaseType_t LOGBUS_PRIO = 1;
        constexpr int         LOGBUS_CORE = 0;

        // esp_http_server, set in WebUi::begin. HTTPD_DEFAULT_CONFIG gives it 5
        // on either core, which put every web request above the loop. It takes
        // no UART mutex and dispatches no key: it posts to the KeyBus and the
        // hu task's out-queue, and stages configs. Keep it that way — a core-0
        // holder of a core-1 lock can be preempted by WiFi, lwIP and NimBLE.
        constexpr UBaseType_t HTTPD_PRIO = 2;
        constexpr int         HTTPD_CORE = 0;

        // The car task (src/app/CarTask.h): the vehicle sources, the MCP2518FD's
        // SPI, the decode into State, the sniffer and the capture hook. 4:
        // below the panel, above everything that only renders or talks to the
        // network. Woken by CAN A's INT or every 2 ms for a polled source.
        // CORE 0 FOR NOW, and to be decided by measurement (plan Step 5): the
        // chip service masks interrupts on its core for the whole FIFO drain
        // ([can2] maskMaxUs). Over 200 us, or BLE links dropping while CAN A
        // is busy, moves it to core 1 at priority 3, below the panel and keys.
        // Not created on a board with no car source.
        constexpr UBaseType_t CAR_PRIO  = 4;  constexpr int CAR_CORE  = 0;
        constexpr uint32_t    CAR_STACK  = 4096;
    }
}
