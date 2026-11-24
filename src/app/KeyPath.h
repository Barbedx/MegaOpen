#pragma once
#include "../Features.h"
#include <Arduino.h>
#include "../cfg/Config.h"
#include "../keys/KeyEngine.h"
#include "../keys/KeyMap.h"
#include "../keys/GestureEngine.h"
#include "../sinks/CanboxUartSink.h"
#include "../utils/Staged.h"
#include "../utils/Snapshot.h"
#include "KeysTask.h"
#include "OneShot.h"
#if FEATURE_RESISTIVE_KEYS
#include "../sinks/ResistiveKeySink.h"
#endif
#if FEATURE_BLE
#include "../sinks/BleKeyProxy.h"
#endif
#if FEATURE_DISPLAY_CAN
#include <AffaDisplay.h>
#include "../keys/AffaMap.h"
#include "../keys/AffaKeySource.h"
#include "../keys/KeyDests.h"
#endif

// Every key object and what the keys and scan tasks run on them
// (docs/design/event-tasks.md, Step 2): the engine, the matrix map and its
// scanner, the sinks (UART, USB HID, the ladder, the BLE proxies), the gesture
// rules' engine and filter, the panel's keys, the web's staged edits, and the
// keys task's hooks (KeysTask.h stays the generic task). Everyone else posts
// to the KeyBus or stages.
namespace App
{
    namespace KeyPath
    {
        // ---- setup(), in this order ------------------------------------------
        // The matrix map from NVS.
        void loadMap(Cfg::Config& cfg);
#if FEATURE_DISPLAY_CAN
        // The stalk map from NVS, and its staged copy.
        void loadPanelMap(Cfg::Config& cfg);
#endif
        // The matrix map and timing, staged; the matrix pins.
        void stage(Cfg::Config& cfg);
        // The scanner, the UART key sink and the routing, staged.
        void beginSinks(Cfg::Config& cfg);
        // The engine, its sinks, the observers and the gesture filter.
        void beginEngine(Cfg::Config& cfg);
#if FEATURE_DISPLAY_CAN
        // The panel's keys onto the KeyBus, mapped on the keys task.
        void attachPanel(affa::AffaDisplayBase& panel);
#endif
        // The keys task: once every sink is registered.
        void start();
        // The scan task (FEATURE_MATRIX).
        void startScan();

        // ---- the objects, for the wiring ---------------------------------------
        Keys::KeyEngine&        engine();
        Keys::KeyMap&           map();
        Sinks::CanboxUartSink&  keySink();
        const uint32_t*         ghosts();
        const uint32_t*         encSteps();
        // Dispatched key events since boot.
        const uint32_t*         engineEvents();
        Staged<KeysTask::Routing>&   routingCfg();
        Staged<KeysTask::KeyMapCfg>& keymapCfg();
        Staged<Keys::GestureTable>&    gestureCfg();
#if FEATURE_RESISTIVE_KEYS
        Sinks::ResistiveKeySink&           resSink();
        Staged<Sinks::ResistiveMap>&       resMapCfg();
        OneShot<uint8_t>&                  resAllOffReq();
        OneShot<KeysTask::ResLearn>&       resLearnReq();
#endif
#if FEATURE_BLE
        Sinks::BleKeyProxy& amsKeys();
        Sinks::BleKeyProxy& hidKeys();
        // Whether each BLE key sink is in the dispatch chain; setup().
        void setBleSinks(bool amsLive, bool hidLive);
        bool amsSinkLive();
        bool hidSinkLive();
        // Volume falls back to the phone now. Any task.
        bool volToAms();
#endif
#if FEATURE_DISPLAY_CAN
        Keys::AffaMap&                 affaMap();
        Keys::AffaKeySource&           affaKeys();
        Staged<Keys::AffaMap>&         affaCfg();
        Snapshot<Keys::PanelKeyLast>&  keyLast();
#endif
    }
}
