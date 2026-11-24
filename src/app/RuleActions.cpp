#include "RuleActions.h"
#include "../Features.h"
#include "KeyBus.h"
#include "HuTask.h"
#include "../keys/GestureEngine.h"
#include "../proto/canbox/Protocol.h"
#include "../utils/Log.h"
#if FEATURE_DISPLAY_CAN
#include "PanelOwner.h"
#include "UiTask.h"
#include "../display/MediaPane.h"
#endif

namespace App
{
    namespace RuleActions
    {
        namespace
        {
#if FEATURE_DISPLAY_CAN
            // `arg` carries up to three panel rows separated by '|'.
            static void gestureRows(const char* arg, char* a, char* b, char* c, size_t cap)
            {
                a[0] = b[0] = c[0] = '\0';
                char* out[3] = { a, b, c };
                uint8_t row = 0, n = 0;
                for (const char* p = arg; *p && row < 3; ++p) {
                    if (*p == '|') { out[row][n] = '\0'; ++row; n = 0; continue; }
                    if (n < cap - 1) out[row][n++] = *p;
                }
                if (row < 3) out[row][n] = '\0';
            }
#endif
        }

        void perform(const Vehicle::RuleAction& a)
        {
            switch (Keys::effectRoute(a.effect)) {
                case Keys::EffectRoute::Keys:
                    if (a.effect == Keys::Effect::Action) {
                        // Through the routing matrix, like a real button.
                        Keys::Event e;
                        e.action   = a.action;
                        e.state    = Keys::State::Press;
                        e.tMs      = millis();
                        e.srcId    = 3;                 // 3 = a rule; 0 matrix, 1 encoder, 2 web
                        e.codeHint = a.code;            // 0 = let the sink use its own profile
                        if (Keys::isEncoder(a.action)) App::KeyBus::postEvent(e);   // a detent has no release
                        else                           App::KeyBus::postTap(e);
                    } else {
                        // KeyCode: one literal code down the UART sink, its release
                        // MIN_GAP_MS later (IKeySink.h).
                        App::KeyBus::postLiteral(a.code, Canbox::KEY_PRESS, true);
                    }
                    break;
                case Keys::EffectRoute::UartOut: {
                    uint8_t frame[Canbox::HU_FRAME_MAX];
                    const uint8_t len = Canbox::activeProtocol().frame(a.code, a.data, a.dataLen, frame);
                    // The hu task's vehicle lane: an arbitrary frame is not a key, and
                    // must not go in front of the next ack.
                    if (!len)
                        LOGI("rule", "payload too long");
                    else if (!App::Hu::post(App::Hu::Kind::Raw, frame, len))
                        LOGI("rule", "uart queue full");
                    break;
                }
                case Keys::EffectRoute::Hu:
                    // Message: the hu task owns the emitter; it reports one with no data.
                    App::Hu::requestSendNow(a.code);
                    break;
#if FEATURE_DISPLAY_CAN
                case Keys::EffectRoute::Panel:
                    if (a.effect == Keys::Effect::Text) {
                        // Static: the car task's stack is small, and it is the one caller.
                        static char l1[MegaOpen::PanelOps::STR_MAX];
                        static char l2[MegaOpen::PanelOps::STR_MAX];
                        static char l3[MegaOpen::PanelOps::STR_MAX];
                        gestureRows(a.arg, l1, l2, l3, sizeof(l1));
                        if (l2[0] || l3[0]) App::Panel::ops().postFullscreen(l1, l2, l3);
                        else                App::Panel::ops().postText(l1);
                    }
                    break;
                case Keys::EffectRoute::Pane:
                    // The pane is the ui task's render state: ask for it.
                    App::Ui::requestScene((uint8_t)MegaOpen::MediaPane::sceneFromName(a.arg));
                    break;
#else
                case Keys::EffectRoute::Panel:
                case Keys::EffectRoute::Pane:
                    LOGI("rule", "this build has no panel - see FEATURE_DISPLAY_CAN");
                    break;
#endif
                default:
                    break;
            }
        }
    }
}
