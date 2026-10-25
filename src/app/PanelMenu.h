#pragma once
#include "../Features.h"
#if FEATURE_DISPLAY_CAN
#include <Arduino.h>
#include "../keys/KeyAction.h"

namespace Cfg { class Config; struct Settings; }
namespace Knobs { struct Config; }
namespace Vehicle { class State; }
namespace MegaOpen { class PanelOps; }

// The panel menu on the car's display (src/ui/MenuEngine.h), across the three
// tasks it touches:
//
//   keys task   filter(): ahead of the macro filter, decides in a few compares
//               whether a key is the menu's (src/ui/MenuInput.h), and if so
//               posts one byte to the ui task. No hop, no wait, no render.
//   ui task     service(): every pass. Runs the menu on what was posted and
//               draws it through PanelOps. While the menu is open the ui task
//               holds the glass (open() is part of its glassHeld()), so the
//               media screen, pane and splash stand down; closing hands the
//               glass back with a Clear.
//   net_svc     applyPending(): a committed value is written into Cfg and the
//               knobs here, staged for the task that owns it and saved, the
//               same task that saves every other setting.
//
// "Power off" switches the panel off; while it is off a press of either knob
// only switches it back on (turns still reach the head unit).
namespace App
{
    namespace PanelMenu
    {
        struct Deps {
            MegaOpen::PanelOps*   ops   = nullptr;
            Cfg::Config*          cfg   = nullptr;
            const Vehicle::State* car   = nullptr;     // null: no Car data row
            // The knobs, or null without them. get: a copy. set: live and saved.
            bool (*knobsGet)(Knobs::Config& out) = nullptr;
            void (*knobsSet)(const Knobs::Config& c) = nullptr;
            // After a change: hand the settings to the task that owns them.
            // ui: the media screen, pane, popups. media: the track-name source.
            void (*stageUi)(const Cfg::Settings& s) = nullptr;
            void (*stageMedia)(const Cfg::Settings& s) = nullptr;
            // The board's address, null: no row in the menu. head = the row
            // ("IP 192.168.1.15"), body = the second line (the mDNS name or
            // 192.168.4.1). ui task. showAddress: the notice, full size.
            void (*address)(char* head, size_t hn, char* body, size_t bn) = nullptr;
            void (*showAddress)() = nullptr;
        };

        // setup(), before the keys and ui tasks start.
        void begin(const Deps& d);

        // ---- keys task -------------------------------------------------------
        // True: the menu took the key; no sink and no macro sees it.
        bool filter(const Keys::Event& e);

        // ---- any task --------------------------------------------------------
        bool isOpen();
        bool panelOff();
        // A steering key's popup would cover the menu while scrolling it:
        // shown only for volume while the menu is open.
        bool popupAllowed(Keys::Action a);

        // ---- ui task -----------------------------------------------------------
        // `ready`: the panel is synced and transmitting. `popupUp`: a popup is
        // on the glass; the menu is redrawn when it goes.
        void service(uint32_t nowMs, bool ready, bool popupUp);

        // ---- net_svc -----------------------------------------------------------
        void applyPending();
    }
}
#endif
