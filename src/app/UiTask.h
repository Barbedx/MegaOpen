#pragma once
#include "../Features.h"
#if FEATURE_DISPLAY_CAN
#include <Arduino.h>
#include "MailboxIds.h"
#include "../display/MediaScreen.h"
#include "../display/MediaPane.h"
#include "../utils/Mirror.h"
#include "../utils/Snapshot.h"
#include "../utils/Staged.h"

class MediaRouter;
class HuMediaSource;
namespace Cfg { class Config; }
namespace Keys { class KeyEngine; }
namespace MegaOpen { class AffaMailbox; class PaneBuffers; }

// The ui task: what goes on the glass, and when (docs/design/event-tasks.md,
// Step 4). The now-playing screen, the 48x48 pane and its carousel, the track
// splash, phone notifications, the clock, the popup's take-down, the glass
// hold, the render breaker, the bring-up probe, the 30 s stuck resync, and the
// phase and registration watch.
//
// It never calls the library. It decides and posts to the mailbox; the panel
// task makes the call and answers through the UiQ (a Completion per tracked
// request) and through PanelStatus, which this task reads every pass. That
// lets it run at priority 2, below everything a key or the panel's timing
// depends on: a 5 ms wait for the AMS mutex, or a slow compose, now costs a
// late repaint and nothing else.
//
// It owns the render state, and nothing outside this file can write it:
//   * other tasks ask (armPopup, requestScene, postHuClock, queueNotification,
//     mediaChanged, splashNow);
//   * the web edits settings() (a Staged<MediaSettings>, applied here with a
//     try-take) and reads copies: status() for the counters, mediaMirror() and
//     paneMirror() for the screen and the pane, refreshed every pass.
namespace App
{
    namespace Ui
    {
        // A one-off edit of what is on screen (the media page's test fields).
        // Holds until the next update from the source. Latest wins.
        struct MediaPoke {
            enum : uint8_t {
                TITLE = 1u << 0, ARTIST = 1u << 1, CLOCK = 1u << 2, STATE = 1u << 3,
                ELAPSED = 1u << 4, DURATION = 1u << 5,
            };
            uint8_t  has = 0;
            char     title[MegaOpen::MediaScreen::ROW_MAX * 2] = {0};
            char     artist[MegaOpen::MediaScreen::ROW_MAX * 2] = {0};
            char     clock[6] = {0};
            uint8_t  state = 0;
            uint16_t elapsed = 0, duration = 0;
        };

        // Everything the web may change about the screen, applied by the ui
        // task as a whole (Staged<T>, §3.1 rule 3). Plain bytes only.
        struct MediaSettings {
            bool     mediaAuto = false;     // the now-playing screen repaints on its own
            uint8_t  scroll = 0x00;         // list indicator, panel encoding
            MegaOpen::MediaScreen::Layout layout = MegaOpen::MediaScreen::Layout::Menu;
            MegaOpen::MediaScreen::Style  style  = MegaOpen::MediaScreen::Style::Classic;
            uint8_t  cols = MegaOpen::MediaScreen::COLS_DEFAULT;
            uint8_t  bar = 10;
            uint16_t scrollMsTitle = 700, scrollMsArtist = 1400;
            // The pane: the carousel, or one scene. sceneSeq moves when the web
            // chooses, so the ui applies a choice once and the carousel can
            // move on from it.
            bool     paneAuto = false;
            MegaOpen::MediaPane::Scene paneScene = MegaOpen::MediaPane::Scene::Off;
            uint16_t sceneSeq = 0;
            uint16_t paneCycleS = 30;
            uint8_t  paneSpeed = 1;
            uint16_t splashMs = 4000;       // 0 = no track splash
            bool     autoTime = true;       // push the clock to the panel
            MediaPoke poke;
            uint16_t pokeSeq = 0;           // moves with each poke
        };

        // The Custom scene's picture (POST /api/media bits=), staged apart from
        // MediaSettings so the settings copies on the httpd stack stay small.
        // `set` is false until a picture was drawn (or loaded from NVS).
        struct PaneBits {
            uint8_t bits[MegaOpen::MediaPane::BYTES] = {0};
            bool    set = false;
        };

        // Who set the panel's clock last (GET /api/media clockSrc).
        enum class ClockSrc : uint8_t { None = 0, Hu, Cts, Manual };
        const char* clockSrcName(ClockSrc s);   // "", "hu", "cts", "manual"

        // The ui task's counters for the web, published every pass.
        struct UiStatus {
            uint32_t panesSent = 0, panesFailed = 0;
            uint32_t glassHeldUntilMs = 0;  // 0 = not held
            uint32_t resyncs = 0;           // registration lost since boot
            uint32_t lastResyncMs = 0;
            uint32_t keyPopups = 0;
            uint8_t  paneScene = 0;         // MediaPane::Scene, live
            ClockSrc clockSrc = ClockSrc::None;
            uint16_t clockAt = 0;           // hours << 8 | minutes of that set
        };

        struct Deps {
            MegaOpen::AffaMailbox*  box    = nullptr;
            MegaOpen::PaneBuffers*  pane   = nullptr;
            MediaRouter*            router = nullptr;
            HuMediaSource*          hu     = nullptr;
            const Keys::KeyEngine*  engine = nullptr;     // events(): the pane's key-quiet gate
            // The phone, without the BLE headers. Null in an image with no radio.
            bool (*amsActive)()      = nullptr;
            bool (*phoneConnected)() = nullptr;
            bool (*phoneTimeSet)()   = nullptr;
        };

        // setup(): the settings from NVS, the objects it draws from, and the
        // UiQ, before any task can post a completion.
        void begin(Cfg::Config& cfg, const Deps& d);
        // The ui task, with a panel only (setup(), after the panel).
        void start();

        // ---- the panel task ------------------------------------------------------
        // A tracked request's verdict. Zero wait; a full UiQ drops and counts,
        // and the ui's 4 s timeout recovers whatever was waiting on it.
        void postCompletion(const MailboxIds::Completion& c);

        // ---- any task: asks ------------------------------------------------------
        void armPopup(uint32_t ms);        // take the popup down this many ms from now
        void releaseGlass();               // hand the glass back to the repaints
        void requestScene(uint8_t scene);  // a macro's pane scene (MediaPane::Scene)
        void postHuClock(uint16_t hm);     // the head unit's 0x9D: hours << 8 | minutes
        // /api/display/clock already posted this time to the panel: record it
        // as the clock and its source, without drawing it again.
        void noteManualClock(uint16_t hm);
        // Sender on row 1, body wrapped over rows 2-3; drawn as a fullscreen.
        void queueNotification(const char* sender, const char* text);
        // The active media source changed; the screen refills on the next pass.
        void mediaChanged();
        // Fire the track splash now, for the current title (the media page).
        void splashNow();

        // The key popup, for the keys task (its settings have one writer, httpd).
        // Track keys get no popup when the track splash will follow.
        bool     keyPopupWanted(bool trackKey);
        uint16_t keyPopupMs();
        // A popup was posted: count it and arm its take-down.
        void     keyPopupShown();

        // The screen repaints on its own (setup's warning; a word read).
        bool mediaAuto();

        // ---- the web -------------------------------------------------------------
        Staged<MediaSettings>&                   settings();
        Staged<PaneBits>&                        paneBits();
        const Snapshot<UiStatus>&                status();
        const Mirror<MegaOpen::MediaScreen>&     mediaMirror();
        const Mirror<MegaOpen::MediaPane>&       paneMirror();

        // Plain words with one writer (httpd), read here and by the keys task;
        // allowed by §3.1 and kept so since Step 2.
        struct Words {
            bool*     keyPopup    = nullptr;
            uint16_t* keyPopupMs  = nullptr;
            uint32_t* glassHoldMs = nullptr;   // 0 = hold until released
        };
        Words words();
    }
}
#endif
