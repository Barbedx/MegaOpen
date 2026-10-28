#include "SettingsRegistry.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../Features.h"
#if FEATURE_DISPLAY_CAN
#include "../display/MediaScreen.h"
#include "../display/MediaPane.h"
#include "../media/MediaRouter.h"
#endif
#include "../bus/Autobaud.h"
#include "../capture/CapFormat.h"
#include "../utils/Log.h"
#include "../utils/LogTags.h"
#include "../net/Catalog.h"
#include "../proto/canbox/Protocol.h"

namespace Cfg
{
    namespace
    {
#if FEATURE_DISPLAY_CAN
        using MegaOpen::MediaScreen;
        using MegaOpen::MediaPane;

        // ---- enum rows -----------------------------------------------------

        // Layouts, in MediaScreen's own row order.
        const SEnum kLayouts = {
            [] { return MediaScreen::LAYOUT_COUNT; },
            [](uint8_t i) { return MediaScreen::layoutAt(i).id; },
            [](uint8_t i) { return MediaScreen::layoutAt(i).name; },
            "layout",
        };

        const SEnum kStyles = {
            [] { return MediaScreen::STYLE_COUNT; },
            [](uint8_t i) { return MediaScreen::styleName((MediaScreen::Style)i); },
            [](uint8_t i) { return MediaScreen::styleTitle((MediaScreen::Style)i); },
            "style",
        };

        // mediaPane: the scenes, then "auto" (the carousel) as the last row,
        // so scene i stays row i.
        const SEnum kPanes = {
            [] { return (uint8_t)(MediaPane::SCENE_COUNT + 1); },
            [](uint8_t i) {
                return i < MediaPane::SCENE_COUNT ? MediaPane::sceneName((MediaPane::Scene)i) : "auto";
            },
            [](uint8_t i) {
                return i < MediaPane::SCENE_COUNT ? MediaPane::sceneTitle((MediaPane::Scene)i) : nullptr;
            },
            "scene",
        };

        // The pickable sources only; "none" is a display row, not a choice.
        uint8_t pickRows()
        {
            uint8_t n = 0;
            for (const auto& r : MediaRouter::SOURCES) if (r.pick) ++n;
            return n;
        }
        const MediaRouter::SourceRow& pickRow(uint8_t i)
        {
            for (const auto& r : MediaRouter::SOURCES) if (r.pick && !i--) return r;
            return MediaRouter::SOURCES[0];
        }
        const SEnum kSources = {
            pickRows,
            [](uint8_t i) { return pickRow(i).id; },
            [](uint8_t i) { return pickRow(i).name; },
            "mediaSrc",
        };

        // A car screen row: row 0 is "none" (SIG_COUNT), then every signal
        // that exists (the built-ins, the user slots with a name), in id
        // order. The id is the signal's ASCII name; the page has its words
        // (k.sig, k.raise; a user signal's name is its own).
        uint8_t sigOfRow(int32_t v)
        {
            for (uint8_t s = 0; s < Vehicle::SIG_COUNT; ++s)
                if (Vehicle::exists(s) && --v == 0) return s;
            return Vehicle::SIG_COUNT;
        }
        int32_t sigRowOf(uint8_t sig)
        {
            if (sig >= Vehicle::SIG_COUNT || !Vehicle::exists(sig)) return 0;
            int32_t r = 0;
            for (uint8_t s = 0; s <= sig; ++s) r += Vehicle::exists(s);
            return r;
        }
        const SEnum kSignals = {
            [] {
                uint8_t n = 1;
                for (uint8_t s = 0; s < Vehicle::SIG_COUNT; ++s) n += Vehicle::exists(s);
                return n;
            },
            [](uint8_t i) { return i ? Vehicle::meta(sigOfRow(i)).name : "none"; },
            [](uint8_t) -> const char* { return nullptr; },   // k.sig / k.raise (web/i18n)
            "sig",
        };

        // Cfg::CarPreset, row = value.
        const SEnum kCarPresets = {
            [] { return (uint8_t)CarPreset::_Count; },
            [](uint8_t i) { return i == (uint8_t)CarPreset::Trip ? "trip" : "custom"; },
            [](uint8_t) -> const char* { return nullptr; },
            "carPreset",
        };
#endif

        const SEnum kBleModes = { Catalog::bleModeCount, Catalog::bleModeId, Catalog::bleModeName, "ble" };
        // Every head-unit protocol in this image (Canbox::Protocols), by id.
        const SEnum kHuProtocols = {
            [] { return Canbox::Protocols::count(); },
            [](uint8_t i) { return Canbox::Protocols::at(i).id(); },
            [](uint8_t) -> const char* { return nullptr; },   // k.proto.<id> (web/i18n)
            "proto",
        };
        const SEnum kPanels   = { Catalog::panelCount,   Catalog::panelId,   Catalog::panelName, "panel" };

        // can2Bitrate: 0 = find it, then the rates Autobaud probes, ascending.
        uint32_t rateRow(uint8_t i)
        {
            if (!i) return 0;
            uint32_t r[Bus::Autobaud::N_RATES];
            for (uint8_t k = 0; k < Bus::Autobaud::N_RATES; ++k) r[k] = Bus::Autobaud::rateAt(k);
            for (uint8_t a = 1; a < Bus::Autobaud::N_RATES; ++a)
                for (uint8_t b = a; b > 0 && r[b] < r[b - 1]; --b) { const uint32_t t = r[b]; r[b] = r[b - 1]; r[b - 1] = t; }
            return i <= Bus::Autobaud::N_RATES ? r[i - 1] : 0;
        }
        const char* rateId(uint8_t i)
        {
            static const char* const kIds[] = {"auto", "125k", "250k", "500k", "1M"};
            const uint32_t r = rateRow(i);
            switch (r) {
                case 0:       return kIds[0];
                case 125000:  return kIds[1];
                case 250000:  return kIds[2];
                case 500000:  return kIds[3];
                case 1000000: return kIds[4];
                default:      return "?";
            }
        }
        uint8_t rateIndex(uint32_t bps)
        {
            for (uint8_t i = 0; i <= Bus::Autobaud::N_RATES; ++i) if (rateRow(i) == bps) return i;
            return 0;
        }

        const SEnum kRates = {
            [] { return (uint8_t)(Bus::Autobaud::N_RATES + 1); },
            rateId,
            [](uint8_t i) -> const char* { return i ? rateId(i) : nullptr; },
            "rate",
        };

        // uartBaud: the rates canbox dialects use; Raise/RAV4 is 38400. A
        // stored rate not in the list reads as no row (BAUDS, out of range:
        // the page shows none chosen), so writing any row, 38400 included,
        // is a change and repairs it; a row would hide the bad rate behind
        // "unchanged" while Serial1 kept running at it.
        const uint32_t kBauds[] = {9600, 19200, 38400, 57600, 115200};
        constexpr uint8_t BAUDS = sizeof(kBauds) / sizeof(kBauds[0]);
        constexpr uint8_t BAUD_DEFAULT_ROW = 2;
        uint8_t baudIndex(uint32_t bps)
        {
            for (uint8_t i = 0; i < BAUDS; ++i) if (kBauds[i] == bps) return i;
            return BAUDS;
        }
        const char* baudId(uint8_t i)
        {
            static const char* const kIds[] = {"9600", "19200", "38400", "57600", "115200"};
            return i < BAUDS ? kIds[i] : kIds[BAUD_DEFAULT_ROW];
        }
        const SEnum kBaudRates = { [] { return BAUDS; }, baudId, baudId };

        // Knob colours: a whole zone in one colour, the menu's way to pick one.
        // Row 0 is whatever the web set LED by LED; it is shown, and choosing
        // it changes nothing.
        struct Preset { const char* id; const char* name; Knobs::Rgb c; };
        const Preset kPresets[] = {
            {"custom", nullptr,        {0, 0, 0}},
            {"white",  nullptr,       {255, 255, 255}},
            {"red",    nullptr,    {255, 0, 0}},
            {"orange", nullptr,   {255, 80, 0}},
            {"amber",  nullptr,     {255, 140, 0}},
            {"yellow", nullptr,      {255, 220, 0}},
            {"green",  nullptr,     {0, 255, 0}},
            {"cyan",   nullptr,   {0, 255, 255}},
            {"blue",   nullptr,       {0, 0, 255}},
            {"purple", nullptr,  {160, 0, 255}},
            {"pink",   nullptr,     {255, 0, 120}},
        };
        constexpr uint8_t PRESETS = sizeof(kPresets) / sizeof(kPresets[0]);
        const SEnum kColors = {
            [] { return PRESETS; },
            [](uint8_t i) { return kPresets[i < PRESETS ? i : 0].id; },
            [](uint8_t i) { return kPresets[i < PRESETS ? i : 0].name; },
            "color",
        };

        bool same(const Knobs::Rgb& a, const Knobs::Rgb& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

        int32_t zoneColor(const Knobs::Zone& z)
        {
            const uint8_t n = z.count && z.count <= Knobs::ZONE_LEDS ? z.count : 1;
            for (uint8_t i = 1; i < n; ++i) if (!same(z.led[i], z.led[0])) return 0;
            for (uint8_t p = 1; p < PRESETS; ++p) if (same(z.led[0], kPresets[p].c)) return p;
            return 0;
        }

        void setZoneColor(Knobs::Zone& z, int32_t v)
        {
            if (v <= 0 || v >= PRESETS) return;     // "custom": keep what is there
            for (auto& l : z.led) l = kPresets[v].c;
        }

#define UI_NAMES_ENUM(T, DOM)                                     \
        SEnum{ [] { return (uint8_t)(sizeof(T) / sizeof(T[0])); }, \
               [](uint8_t i) { return T[i].id; },                  \
               [](uint8_t i) { return T[i].name; }, DOM }
        const SEnum kReact = UI_NAMES_ENUM(Knobs::REACT_NAMES, "ledReact");
        const SEnum kAnim  = UI_NAMES_ENUM(Knobs::ANIM_NAMES, "ledIdle");
        const SEnum kConds = UI_NAMES_ENUM(Vehicle::ALERT_COND, "cond");
        const SEnum kCapModes = UI_NAMES_ENUM(CapFormat::MODE_NAMES, "capModes");

        // Log levels: row i = LogLevel i, the id is also the name.
        const SEnum kLogLevels = {
            [] { return Log::LEVELS; },
            [](uint8_t i) { return Log::LEVEL_NAMES[i < Log::LEVELS ? i : 0]; },
            [](uint8_t i) { return Log::LEVEL_NAMES[i < Log::LEVELS ? i : 0]; },
        };
        // The status lines' interval at INF (Log::setStatusInterval).
        const UiName kLogStatusNames[] = {{"10", "10 s"}, {"60", "60 s"}, {"300", "300 s"}};
        const SEnum kLogStatus = {
            [] { return (uint8_t)3; },
            [](uint8_t i) { return kLogStatusNames[i < 3 ? i : 1].id; },
            [](uint8_t i) { return kLogStatusNames[i < 3 ? i : 1].name; },
        };
        // logTags: kept only when it reads (LogTags::setSpec), applied at once.
        const SStr kLogTags = {
            (uint8_t)LogTags::SPEC_MAX,
            [](const Model& m, char* out, size_t cap) {
                if (cap) snprintf(out, cap, "%s", m.s->logTags.c_str());
            },
            [](Model& m, const char* v) {
                LogTags probe;
                if (!probe.setSpec(v)) return;
                m.s->logTags = v;
                Log::setTagSpec(v);
            },
        };
#undef UI_NAMES_ENUM

        // ---- the panel alerts (Vehicle::AlertRules) -------------------------
        // A rule's signal: built-in signal i is row i, and the row after them,
        // the last, is an empty rule ("", stored as SIG_COUNT). The user
        // signals have no alert: rules take alerts over (rules.md, slice 4).
        constexpr uint8_t ALERT_EMPTY = Vehicle::SIG_USER_0;
        const SEnum kAlertSigs = {
            [] { return (uint8_t)(ALERT_EMPTY + 1); },
            [](uint8_t i) { return i < ALERT_EMPTY ? Vehicle::meta(i).name : ""; },
            [](uint8_t i) -> const char* { return i >= ALERT_EMPTY ? "—" : nullptr; },
            "sig",
        };

        // AlertRules::Col order. thr's range is its row's signal's (the
        // table refuses the rest); the page offers any number.
        const SColumn kAlertCols[] = {
            {"sig",       SType::Enum, 0, 0, &kAlertSigs},
            {"cond",      SType::Enum, 0, 0, &kConds},
            {"thr",       SType::Int,  INT32_MIN, INT32_MAX, nullptr},
            {"cooldownS", SType::Int,  Vehicle::ALERT_COOLDOWN_MIN_S, 65535, nullptr},
            {"on",        SType::Bool, 0, 1, nullptr},
        };
        static_assert(sizeof(kAlertCols) / sizeof(kAlertCols[0]) == Vehicle::AlertRules::COLS,
                      "kAlertCols is out of step with AlertRules::Col");

        const STable kAlerts = {
            [] { return Vehicle::AlertRules::ROWS; }, Vehicle::AlertRules::COLS, kAlertCols,
            [](const Model& m, uint8_t r, uint8_t c) {
                const int32_t v = m.a->get(r, c);
                return c == Vehicle::AlertRules::C_SIG && v >= ALERT_EMPTY ? (int32_t)ALERT_EMPTY : v;
            },
            [](Model& m, uint8_t r, uint8_t c, int32_t v) {
                if (c == Vehicle::AlertRules::C_SIG && v >= ALERT_EMPTY) v = Vehicle::SIG_COUNT;
                // An empty rule is never on (the /api/map rule before).
                if (c == Vehicle::AlertRules::C_ON && m.a->r[r].sig >= Vehicle::SIG_COUNT) v = 0;
                return m.a->set(r, c, v);
            },
            [](const Model& m, uint8_t r, uint8_t c) -> uint8_t {
                const uint8_t sig = m.a->r[r].sig;
                return c == Vehicle::AlertRules::C_THR && sig < Vehicle::SIG_COUNT ? Vehicle::meta(sig).decimals : 0;
            },
        };

        // ---- the head-unit emitter's messages: row i = message i, on/off ----
        const SColumn kMsgCols[] = {
            {"on", SType::Bool, 0, 1, nullptr},
        };
        const STable kMessages = {
            [] { return Canbox::activeProtocol().messageCount(); }, 1, kMsgCols,
            [](const Model& m, uint8_t r, uint8_t) -> int32_t { return m.msg[r] ? 1 : 0; },
            [](Model& m, uint8_t r, uint8_t, int32_t v) { m.msg[r] = v != 0; return true; },
            nullptr,
        };
        static_assert(Canbox::HuConfig::MAX_MESSAGES <= 255, "a table has at most 255 rows");

        // A String field stored by id, read and written as its row.
        int32_t rowOf(const SEnum& en, const Str& v)
        {
            const uint8_t n = en.count();
            for (uint8_t i = 0; i < n; ++i) if (!strcmp(v.c_str(), en.id(i))) return i;
            return 0;
        }

        constexpr uint8_t W  = F_WEB;
        constexpr uint8_t WM = F_WEB | F_MENU;

        // Shorthands for the table. G = a group, B/I/E = Bool/Int/Enum.
        constexpr SEntry G(const char* id, const char* tag, const char* parent, uint8_t flags)
        {
            return SEntry{id, tag, parent, SType::Group, flags, AP_NONE,
                          0, 0, 0, nullptr, nullptr, nullptr, nullptr};
        }

        // Names: the Settings field `f` via s, a knobs field via k.
#define S_GET(expr) [](const Model& m) -> int32_t { const Settings& s = *m.s; return (int32_t)(expr); }
#define S_PUT(stmt) [](Model& m, int32_t v) { Settings& s = *m.s; (void)v; stmt; }
#define K_GET(expr) [](const Model& m) -> int32_t { const Knobs::Config& k = *m.k; return (int32_t)(expr); }
#define K_PUT(stmt) [](Model& m, int32_t v) { Knobs::Config& k = *m.k; (void)v; stmt; }
#define C_GET(expr) [](const Model& m) -> int32_t { const Capture::Settings& c = *m.c; return (int32_t)(expr); }
#define C_PUT(stmt) [](Model& m, int32_t v) { Capture::Settings& c = *m.c; (void)v; stmt; }

        const SEntry kTable[] = {
#if FEATURE_DISPLAY_CAN    // the screen: nothing to set on a build without the panel
            // ---- the screen ------------------------------------------------
            G("display", "Display", nullptr, WM),
            {"mediaScreen", "Audio screen", "display", SType::Bool, WM, AP_UI,
             0, 1, 0, nullptr, nullptr, S_GET(s.mediaScreen), S_PUT(s.mediaScreen = v != 0)},
            // Display › Car screen: on/off, the preset, the custom rows.
            G("display.car", "Car screen", "display", WM),
            {"carScreen", "Show", "display.car", SType::Bool, WM, AP_UI,
             0, 1, 0, nullptr, nullptr, S_GET(s.carScreen), S_PUT(s.carScreen = v != 0)},
            {"carPreset", "Preset", "display.car", SType::Enum, WM, AP_UI,
             0, 0, 0, nullptr, &kCarPresets, S_GET(s.carPreset), S_PUT(s.carPreset = (uint8_t)v)},
            {"carSig1", "Car row 1", "display.car", SType::Enum, WM, AP_UI,
             0, 0, 0, nullptr, &kSignals, S_GET(sigRowOf(s.carSig[0])), S_PUT(s.carSig[0] = sigOfRow(v))},
            {"carSig2", "Car row 2", "display.car", SType::Enum, WM, AP_UI,
             0, 0, 0, nullptr, &kSignals, S_GET(sigRowOf(s.carSig[1])), S_PUT(s.carSig[1] = sigOfRow(v))},
            {"carSig3", "Car row 3", "display.car", SType::Enum, WM, AP_UI,
             0, 0, 0, nullptr, &kSignals, S_GET(sigRowOf(s.carSig[2])), S_PUT(s.carSig[2] = sigOfRow(v))},
            {"mediaLayout", "Layout", "display", SType::Enum, WM, AP_UI,
             0, 0, 0, nullptr, &kLayouts, S_GET(rowOf(kLayouts, s.mediaLayout)),
             S_PUT(s.mediaLayout = kLayouts.id((uint8_t)v))},
            {"mediaStyle", "Text style", "display", SType::Enum, WM, AP_UI,
             0, 0, 0, nullptr, &kStyles, S_GET(rowOf(kStyles, s.mediaStyle)),
             S_PUT(s.mediaStyle = kStyles.id((uint8_t)v))},
            {"mediaPane", "Scene", "display", SType::Enum, WM, AP_UI,
             0, 0, 0, nullptr, &kPanes, S_GET(rowOf(kPanes, s.mediaPane)),
             S_PUT(s.mediaPane = kPanes.id((uint8_t)v))},
            // 5..600 s per stop (the /api/media rule).
            {"paneCycleS", "Carousel", "display", SType::Int, WM, AP_UI,
             5, 600, 5, "s", nullptr, S_GET(s.paneCycleS), S_PUT(s.paneCycleS = (uint16_t)v)},
            // 1..6: past that it stops reading as motion at two images a second.
            {"paneSpeed", "Scene speed", "display", SType::Int, WM, AP_UI,
             1, 6, 1, nullptr, nullptr, S_GET(s.paneSpeed), S_PUT(s.paneSpeed = (uint8_t)v)},
            // 0 = off, else 1..20 s; a value under a second becomes one.
            {"trackSplashMs", "Track splash", "display", SType::Int, WM, AP_UI,
             0, 20000, 1000, "ms", nullptr, S_GET(s.trackSplashMs),
             S_PUT(s.trackSplashMs = (uint16_t)(v <= 0 ? 0 : (v < 1000 ? 1000 : v)))},
            {"autoTime", "Phone clock", "display", SType::Bool, WM, AP_UI,
             0, 1, 0, nullptr, nullptr, S_GET(s.autoTime), S_PUT(s.autoTime = v != 0)},
            {"keyPopup", "Key popup", "display", SType::Bool, WM, AP_UI,
             0, 1, 0, nullptr, nullptr, S_GET(s.keyPopup), S_PUT(s.keyPopup = v != 0)},
            // Shorter than a glance, or longer than a screen, is refused.
            {"keyPopupMs", "Popup time", "display", SType::Int, WM, AP_UI,
             300, 10000, 100, "ms", nullptr, S_GET(s.keyPopupMs), S_PUT(s.keyPopupMs = (uint16_t)v)},
            {"ancsPopup", "Notifications", "display", SType::Bool, WM, AP_UI,
             0, 1, 0, nullptr, nullptr, S_GET(s.ancsPopup), S_PUT(s.ancsPopup = v != 0)},
            {"mediaSource", "Track source", "display", SType::Enum, WM, AP_MEDIA,
             0, 0, 0, nullptr, &kSources, S_GET(rowOf(kSources, s.mediaSource)),
             S_PUT(s.mediaSource = kSources.id((uint8_t)v))},
#endif

            // ---- the knob lights -------------------------------------------
            G("lights", "Lights", nullptr, WM),
            {"ledEnabled", "Lights on", "lights", SType::Bool, WM, AP_KNOBS,
             0, 1, 0, nullptr, nullptr, K_GET(k.ledEnabled), K_PUT(k.ledEnabled = v != 0)},
            {"ledBrightness", "Brightness", "lights", SType::Int, WM | F_LIVE, AP_KNOBS,
             0, 255, 15, nullptr, nullptr, K_GET(k.brightness), K_PUT(k.brightness = (uint8_t)v)},
            // The instrument illumination off the car's CAN as the master
            // brightness (Knobs::Config::dashFollow), scaled by the gain.
            {"ledDash", "Follow dash", "lights", SType::Bool, WM, AP_KNOBS,
             0, 1, 0, nullptr, nullptr, K_GET(k.dashFollow), K_PUT(k.dashFollow = v != 0)},
            {"ledDashGain", "Dash gain", "lights", SType::Int, WM | F_LIVE, AP_KNOBS,
             Knobs::Config::DASH_GAIN_MIN, Knobs::Config::DASH_GAIN_MAX, 10, "%", nullptr,
             K_GET(k.dashGain), K_PUT(k.dashGain = (uint8_t)v)},
            G("lights.r", "Right knob", "lights", WM),
            {"ledR.react", "Reaction", "lights.r", SType::Enum, WM, AP_KNOBS,
             0, 0, 0, nullptr, &kReact, K_GET(k.zone[Knobs::ZONE_RIGHT].react),
             K_PUT(k.zone[Knobs::ZONE_RIGHT].react = (Knobs::React)v)},
            {"ledR.anim", "Idle", "lights.r", SType::Enum, WM, AP_KNOBS,
             0, 0, 0, nullptr, &kAnim, K_GET(k.zone[Knobs::ZONE_RIGHT].anim),
             K_PUT(k.zone[Knobs::ZONE_RIGHT].anim = (Knobs::Anim)v)},
            {"ledR.color", "Colour", "lights.r", SType::Enum, WM, AP_KNOBS,
             0, 0, 0, nullptr, &kColors, K_GET(zoneColor(k.zone[Knobs::ZONE_RIGHT])),
             K_PUT(setZoneColor(k.zone[Knobs::ZONE_RIGHT], v))},
            G("lights.l", "Left knob", "lights", WM),
            {"ledL.react", "Reaction", "lights.l", SType::Enum, WM, AP_KNOBS,
             0, 0, 0, nullptr, &kReact, K_GET(k.zone[Knobs::ZONE_LEFT].react),
             K_PUT(k.zone[Knobs::ZONE_LEFT].react = (Knobs::React)v)},
            {"ledL.anim", "Idle", "lights.l", SType::Enum, WM, AP_KNOBS,
             0, 0, 0, nullptr, &kAnim, K_GET(k.zone[Knobs::ZONE_LEFT].anim),
             K_PUT(k.zone[Knobs::ZONE_LEFT].anim = (Knobs::Anim)v)},
            {"ledL.color", "Colour", "lights.l", SType::Enum, WM, AP_KNOBS,
             0, 0, 0, nullptr, &kColors, K_GET(zoneColor(k.zone[Knobs::ZONE_LEFT])),
             K_PUT(setZoneColor(k.zone[Knobs::ZONE_LEFT], v))},

            // ---- keys and the head-unit link (web only) ----------------------
            // The timing ranges are wide on purpose: the debouncer had no bounds
            // before, so they only stop nonsense.
            G("keys", "Keys", nullptr, W),
            {"debounceMs", "Debounce", "keys", SType::Int, W, AP_KEYS,
             1, 200, 1, "ms", nullptr, S_GET(s.timing.debounceMs), S_PUT(s.timing.debounceMs = (uint16_t)v)},
            {"fastDebounceMs", "Fast debounce", "keys", SType::Int, W, AP_KEYS,
             0, 50, 1, "ms", nullptr, S_GET(s.timing.fastDebounceMs), S_PUT(s.timing.fastDebounceMs = (uint16_t)v)},
            {"longMs", "Long press", "keys", SType::Int, W, AP_KEYS,
             200, 5000, 50, "ms", nullptr, S_GET(s.timing.longMs), S_PUT(s.timing.longMs = (uint16_t)v)},
            {"repeatMs", "Repeat", "keys", SType::Int, W, AP_KEYS,
             50, 2000, 10, "ms", nullptr, S_GET(s.timing.repeatMs), S_PUT(s.timing.repeatMs = (uint16_t)v)},
            {"repeatEnabled", "Repeat on", "keys", SType::Bool, W, AP_KEYS,
             0, 1, 0, nullptr, nullptr, S_GET(s.timing.repeatEnabled), S_PUT(s.timing.repeatEnabled = v != 0)},
            {"encInverted", "Enc reversed", "keys", SType::Bool, W, AP_KEYS,
             0, 1, 0, nullptr, nullptr, S_GET(s.encInverted), S_PUT(s.encInverted = v != 0)},
            // Destination class masks (Keys::CLASS_*).
            {"routeUart", "Route UART", "keys", SType::Int, W, AP_KEYS,
             0, Keys::CLASS_ALL, 1, nullptr, nullptr, S_GET(s.routeUart), S_PUT(s.routeUart = (uint8_t)v)},
            {"routeAms", "Route AMS", "keys", SType::Int, W, AP_KEYS,
             0, Keys::CLASS_ALL, 1, nullptr, nullptr, S_GET(s.routeAms), S_PUT(s.routeAms = (uint8_t)v)},
            {"routeHid", "Route HID", "keys", SType::Int, W, AP_KEYS,
             0, Keys::CLASS_ALL, 1, nullptr, nullptr, S_GET(s.routeHid), S_PUT(s.routeHid = (uint8_t)v)},
            {"routeRes", "Route RES", "keys", SType::Int, W, AP_KEYS,
             0, Keys::CLASS_ALL, 1, nullptr, nullptr, S_GET(s.routeRes), S_PUT(s.routeRes = (uint8_t)v)},
            {"routeUsb", "Route USB", "keys", SType::Int, W, AP_KEYS,
             0, Keys::CLASS_ALL, 1, nullptr, nullptr, S_GET(s.routeUsb), S_PUT(s.routeUsb = (uint8_t)v)},

            G("link", "Head unit", nullptr, W),
            {"uartEnabled", "UART on", "link", SType::Bool, W, AP_KEYS,
             0, 1, 0, nullptr, nullptr, S_GET(s.uartEnabled), S_PUT(s.uartEnabled = v != 0)},
            {"pulseMs", "Key pulse", "link", SType::Int, W, AP_KEYS,
             40, 500, 10, "ms", nullptr, S_GET(s.pulseMs), S_PUT(s.pulseMs = (uint16_t)v)},
            // Picked once at boot (Canbox::Protocols::select).
            {"huProtocol", "HU protocol", "link", SType::Enum, W, AP_REBOOT,
             0, 0, 0, nullptr, &kHuProtocols, S_GET(rowOf(kHuProtocols, s.huProtocol)),
             S_PUT(s.huProtocol = kHuProtocols.id((uint8_t)v))},
            // Serial1 is opened once at boot.
            {"uartBaud", "UART baud", "link", SType::Enum, W, AP_REBOOT,
             0, 0, 0, nullptr, &kBaudRates, S_GET(baudIndex(s.uartBaud)),
             S_PUT(s.uartBaud = kBauds[v < BAUDS ? v : BAUD_DEFAULT_ROW])},
            {"resEnabled", "KEY1/KEY2", "link", SType::Bool, W, AP_KEYS,
             0, 1, 0, nullptr, nullptr, S_GET(s.resEnabled), S_PUT(s.resEnabled = v != 0)},
            {"huTitleFirst", "Title first", "link", SType::Bool, W, AP_MEDIA,
             0, 1, 0, nullptr, nullptr, S_GET(s.huTitleFirst), S_PUT(s.huTitleFirst = v != 0)},

            // ---- read at boot ----------------------------------------------
            G("system", "System", nullptr, W),
            {"bleMode", "Bluetooth", "system", SType::Enum, W, AP_REBOOT,
             0, 0, 0, nullptr, &kBleModes, S_GET(rowOf(kBleModes, s.bleMode)),
             S_PUT(s.bleMode = kBleModes.id((uint8_t)v))},
            {"displayType", "Panel", "system", SType::Enum, W, AP_REBOOT,
             0, 0, 0, nullptr, &kPanels, S_GET(rowOf(kPanels, s.displayType)),
             S_PUT(s.displayType = kPanels.id((uint8_t)v))},
            {"funcReg", "FuncsReg", "system", SType::Bool, W, AP_REBOOT,
             0, 1, 0, nullptr, nullptr, S_GET(s.funcReg), S_PUT(s.funcReg = v != 0)},
            {"mdnsOn", "mDNS", "system", SType::Bool, W, AP_REBOOT,
             0, 1, 0, nullptr, nullptr, S_GET(s.mdnsOn), S_PUT(s.mdnsOn = v != 0)},
            // Live: WifiManager reads it every pass (net_svc).
            {"wifiAuto", "No WiFi on USB", "system", SType::Bool, W, AP_NONE,
             0, 1, 0, nullptr, nullptr, S_GET(s.wifiAuto), S_PUT(s.wifiAuto = v != 0)},
            {"can2Bitrate", "CAN A rate", "system", SType::Enum, W, AP_REBOOT,
             0, 0, 0, nullptr, &kRates, S_GET(rateIndex(s.can2Bitrate)),
             S_PUT(s.can2Bitrate = rateRow((uint8_t)v))},
            // Written by net_svc when the search locks on; shown only.
            {"can2Detected", "CAN A found", "system", SType::Enum, W | F_RO, AP_NONE,
             0, 0, 0, nullptr, &kRates, S_GET(rateIndex(s.can2Detected)),
             S_PUT(s.can2Detected = rateRow((uint8_t)v))},

            // ---- service: RAM only ------------------------------------------
            // TRC costs the CAN path real time, so the level is never saved:
            // every boot starts at INF.
            // The driving reminders (src/vehicle/Derived.h): live, the car task
            // takes them within a heartbeat (hkBeat). 0 switches one off.
            G("remind", "Reminders", nullptr, W),
            {"remindLightsKmh", "No lights km/h", "remind", SType::Int, W, AP_NONE,
             0, 60, 1, "km/h", nullptr, S_GET(s.remindLightsKmh), S_PUT(s.remindLightsKmh = (uint8_t)v)},
            {"remindBrakeKmh", "Handbrake km/h", "remind", SType::Int, W, AP_NONE,
             0, 60, 1, "km/h", nullptr, S_GET(s.remindBrakeKmh), S_PUT(s.remindBrakeKmh = (uint8_t)v)},

            // The gear table (src/vehicle/Derived.h): km/h per 1000 rpm x10,
            // live, the car task reads it like the reminders' speeds.
            G("gears", "Gears", nullptr, W),
            {"gear1", "1st", "gears", SType::Int, W, AP_NONE,
             0, 999, 1, nullptr, nullptr, S_GET(s.gearRatio[0]), S_PUT(s.gearRatio[0] = (uint16_t)v)},
            {"gear2", "2nd", "gears", SType::Int, W, AP_NONE,
             0, 999, 1, nullptr, nullptr, S_GET(s.gearRatio[1]), S_PUT(s.gearRatio[1] = (uint16_t)v)},
            {"gear3", "3rd", "gears", SType::Int, W, AP_NONE,
             0, 999, 1, nullptr, nullptr, S_GET(s.gearRatio[2]), S_PUT(s.gearRatio[2] = (uint16_t)v)},
            {"gear4", "4th", "gears", SType::Int, W, AP_NONE,
             0, 999, 1, nullptr, nullptr, S_GET(s.gearRatio[3]), S_PUT(s.gearRatio[3] = (uint16_t)v)},
            {"gear5", "5th", "gears", SType::Int, W, AP_NONE,
             0, 999, 1, nullptr, nullptr, S_GET(s.gearRatio[4]), S_PUT(s.gearRatio[4] = (uint16_t)v)},
            {"gear6", "6th", "gears", SType::Int, W, AP_NONE,
             0, 999, 1, nullptr, nullptr, S_GET(s.gearRatio[5]), S_PUT(s.gearRatio[5] = (uint16_t)v)},
            G("service", "Service", nullptr, WM),
            // The log (docs/design/logging.md): saved, applied at once.
            {"logLevel", "Log level", "service", SType::Enum, WM, AP_NONE,
             0, 0, 0, nullptr, &kLogLevels, S_GET(s.logLevel),
             [](Model& m, int32_t v) { m.s->logLevel = (uint8_t)v; Log::setLevel((LogLevel)v); }},
            {"logTags", "Log per tag", "service", SType::Str, W, AP_NONE,
             0, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr, &kLogTags},
            {"logWeb", "Log to page", "service", SType::Enum, W, AP_NONE,
             0, 0, 0, nullptr, &kLogLevels, S_GET(s.logWeb),
             [](Model& m, int32_t v) { m.s->logWeb = (uint8_t)v; Log::setSinkLevel(Log::SINK_WEB, (LogLevel)v); }},
            {"logFlash", "Log to flash", "service", SType::Enum, W, AP_NONE,
             0, 3, 0, nullptr, &kLogLevels, S_GET(s.logFlash),
             [](Model& m, int32_t v) { m.s->logFlash = (uint8_t)v; Log::setSinkLevel(Log::SINK_FLASH, (LogLevel)v); }},
            {"logPush", "Log to GitHub", "service", SType::Enum, W, AP_NONE,
             0, 0, 0, nullptr, &kLogLevels, S_GET(s.logPush),
             [](Model& m, int32_t v) { m.s->logPush = (uint8_t)v; Log::setSinkLevel(Log::SINK_PUSH, (LogLevel)v); }},
            {"logStatus", "Status every", "service", SType::Enum, W, AP_NONE,
             0, 0, 0, nullptr, &kLogStatus, S_GET(s.logStatus),
             [](Model& m, int32_t v) {
                 static const uint32_t MS[3] = {10000, 60000, 300000};
                 m.s->logStatus = (uint8_t)(v < 3 ? v : 1);
                 Log::setStatusInterval(MS[m.s->logStatus]);
             }},
            {"logSerial", "Log to serial", "service", SType::Enum, W, AP_NONE,
             0, 0, 0, nullptr, &kLogLevels, S_GET(s.logSerial),
             [](Model& m, int32_t v) { m.s->logSerial = (uint8_t)v; Log::setSinkLevel(Log::SINK_SERIAL, (LogLevel)v); }},
            // Set by the page's «Режим розробника»; off at boot.
            {"devMode", "Dev mode", "service", SType::Bool, W | F_VOLATILE, AP_NONE,
             0, 1, 0, nullptr, nullptr, S_GET(s.devMode), S_PUT(s.devMode = v != 0)},

            // ---- the CAN capture recorder (its own NVS namespace) -------------
            // Ranges are Capture::setSettings()'s clamps.
            G("cap", "Capture", nullptr, W),
            {"cap.autostart", "Autostart", "cap", SType::Bool, W, AP_CAP,
             0, 1, 0, nullptr, nullptr, C_GET(c.autostart), C_PUT(c.autostart = v != 0)},
            {"cap.mode", "Mode", "cap", SType::Enum, W, AP_CAP,
             0, 0, 0, nullptr, &kCapModes, C_GET(c.mode), C_PUT(c.mode = (uint8_t)v)},
            {"cap.ring", "Ring", "cap", SType::Bool, W, AP_CAP,
             0, 1, 0, nullptr, nullptr, C_GET(c.ring), C_PUT(c.ring = v != 0)},
            {"cap.fill", "Fill", "cap", SType::Int, W, AP_CAP,
             10, 95, 5, "%", nullptr, C_GET(c.fillPct), C_PUT(c.fillPct = (uint8_t)v)},
            {"cap.fileMB", "File size", "cap", SType::Int, W, AP_CAP,
             1, 8, 1, "MB", nullptr, C_GET(c.fileMB), C_PUT(c.fileMB = (uint8_t)v)},
            {"cap.autoProtect", "Mark protects", "cap", SType::Bool, W, AP_CAP,
             0, 1, 0, nullptr, nullptr, C_GET(c.autoProtect), C_PUT(c.autoProtect = v != 0)},

            // ---- the panel alerts: the whole table, row i = rule i ----------
            // Drawn by the live-car page, not the settings card.
            {"alerts", "Alerts", nullptr, SType::Table, W, AP_CAR,
             0, 0, 0, nullptr, nullptr, nullptr, nullptr, &kAlerts},

            // ---- which Raise messages go to the head unit: RAM only ---------
            // A bench switch (every boot sends them all); drawn by the service
            // page next to each message's live bytes (/api/messages).
            {"huMessages", "HU messages", nullptr, SType::Table, W | F_VOLATILE, AP_HU,
             0, 0, 0, nullptr, nullptr, nullptr, nullptr, &kMessages},
        };
#undef S_GET
#undef S_PUT
#undef K_GET
#undef K_PUT
#undef C_GET
#undef C_PUT

        constexpr size_t N = sizeof(kTable) / sizeof(kTable[0]);
    }

    namespace Registry
    {
        // The table, then the head-unit protocol's own rows (raise.*).
        size_t count() { return N + Canbox::activeProtocol().settingCount(); }
        const SEntry& at(size_t i)
        {
            if (i < N) return kTable[i];
            const Canbox::Protocol& p = Canbox::activeProtocol();
            return i - N < p.settingCount() ? p.setting(i - N) : kTable[0];
        }

        int indexOf(const char* id)
        {
            const size_t n = count();
            if (id) for (size_t i = 0; i < n; ++i) if (!strcmp(id, at(i).id)) return (int)i;
            return -1;
        }

        const SEntry* find(const char* id)
        {
            const int i = indexOf(id);
            return i < 0 ? nullptr : &at((size_t)i);
        }

        bool available(const Model& m, const SEntry& e)
        {
            if (e.type == SType::Group) return true;
            if (e.apply & AP_CAR) return m.a != nullptr;
            if (e.apply & AP_CAP) return m.c != nullptr;
            if (e.apply & AP_HU)  return m.hu != nullptr && m.msg != nullptr;
            return (e.apply & AP_KNOBS) ? m.k != nullptr : m.s != nullptr;
        }

        int32_t minOf(const SEntry& e) { return e.type == SType::Int ? e.min : 0; }

        int32_t maxOf(const SEntry& e)
        {
            switch (e.type) {
                case SType::Int:  return e.max;
                case SType::Bool: return 1;
                case SType::Enum: return e.en ? (int32_t)e.en->count() - 1 : 0;
                default:          return 0;
            }
        }

        bool valid(const SEntry& e, int32_t v)
        {
            return e.type != SType::Group && e.type != SType::Table && e.type != SType::Str &&
                   v >= minOf(e) && v <= maxOf(e);
        }

        int32_t clamp(const SEntry& e, int32_t v)
        {
            const int32_t lo = minOf(e), hi = maxOf(e);
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            if (e.type == SType::Int && e.step > 1) {
                v = lo + (v - lo) / e.step * e.step;
            }
            return v;
        }

        int32_t get(const Model& m, const SEntry& e)
        {
            return available(m, e) && e.get ? e.get(m) : 0;
        }

        bool set(Model& m, const SEntry& e, int32_t v, uint8_t* apply)
        {
            if ((e.flags & F_RO) || !e.put || !available(m, e) || !valid(e, v)) return false;
            if (e.get(m) == v) return true;
            e.put(m, v);
            if (apply) *apply |= e.apply;
            return true;
        }

        int32_t stepped(const SEntry& e, int32_t v, int8_t dir)
        {
            const int32_t lo = minOf(e), hi = maxOf(e);
            if (e.type == SType::Int) {
                const int32_t step = e.step ? e.step : 1;
                return clamp(e, v + (dir < 0 ? -step : step));
            }
            if (hi <= lo) return lo;
            v += dir < 0 ? -1 : 1;
            if (v > hi) v = lo;
            if (v < lo) v = hi;
            return v;
        }

        int enumIndex(const SEntry& e, const char* id)
        {
            if (e.type != SType::Enum || !e.en || !id) return -1;
            const uint8_t n = e.en->count();
            for (uint8_t i = 0; i < n; ++i) if (!strcmp(id, e.en->id(i))) return i;
            return -1;
        }

        const char* enumId(const SEntry& e, int32_t v)
        {
            return valid(e, v) && e.en ? e.en->id((uint8_t)v) : "";
        }

        const char* enumName(const SEntry& e, int32_t v)
        {
            return valid(e, v) && e.en ? e.en->name((uint8_t)v) : "";
        }

        void format(const Model& m, const SEntry& e, char* out, size_t cap)
        {
            if (!cap) return;
            out[0] = 0;
            if (!available(m, e)) { snprintf(out, cap, "--"); return; }
            formatValue(e, get(m, e), out, cap);
        }

        void formatValue(const SEntry& e, int32_t v, char* out, size_t cap)
        {
            if (!cap) return;
            out[0] = 0;
            switch (e.type) {
                case SType::Bool: snprintf(out, cap, "%s", v ? "On" : "Off"); break;
                case SType::Enum: snprintf(out, cap, "%s", enumId(e, v)); break;
                case SType::Int:
                    if (e.unit) snprintf(out, cap, "%ld%s", (long)v, e.unit);
                    else        snprintf(out, cap, "%ld", (long)v);
                    break;
                default: break;
            }
        }

        // ---- Table cells ---------------------------------------------------

        int32_t colMin(const SColumn& c) { return c.type == SType::Int ? c.min : 0; }

        int32_t colMax(const SColumn& c)
        {
            switch (c.type) {
                case SType::Int:  return c.max;
                case SType::Bool: return 1;
                case SType::Enum: return c.en ? (int32_t)c.en->count() - 1 : 0;
                default:          return 0;
            }
        }

        int colEnumIndex(const SColumn& c, const char* id)
        {
            if (c.type != SType::Enum || !c.en || !id) return -1;
            const uint8_t n = c.en->count();
            for (uint8_t i = 0; i < n; ++i) if (!strcmp(id, c.en->id(i))) return i;
            return -1;
        }

        namespace
        {
            bool inTable(const Model& m, const SEntry& e, uint8_t row, uint8_t col)
            {
                return e.type == SType::Table && e.tab && row < e.tab->rows() && col < e.tab->cols &&
                       available(m, e);
            }
        }

        int32_t cell(const Model& m, const SEntry& e, uint8_t row, uint8_t col)
        {
            return inTable(m, e, row, col) ? e.tab->get(m, row, col) : 0;
        }

        bool setCell(Model& m, const SEntry& e, uint8_t row, uint8_t col, int32_t v, uint8_t* apply)
        {
            if ((e.flags & F_RO) || !inTable(m, e, row, col)) return false;
            const SColumn& c = e.tab->col[col];
            if (c.type != SType::Int && (v < colMin(c) || v > colMax(c))) return false;
            if (e.tab->get(m, row, col) == v) return true;
            if (!e.tab->put(m, row, col, v)) return false;
            if (apply) *apply |= e.apply;
            return true;
        }

        uint8_t cellDecimals(const Model& m, const SEntry& e, uint8_t row, uint8_t col)
        {
            return inTable(m, e, row, col) && e.tab->decimals ? e.tab->decimals(m, row, col) : 0;
        }

        // ---- Str -----------------------------------------------------------

        void getText(const Model& m, const SEntry& e, char* out, size_t cap)
        {
            if (!cap) return;
            out[0] = 0;
            if (e.type == SType::Str && e.str && available(m, e)) e.str->get(m, out, cap);
        }

        bool validText(const SEntry& e, const char* v)
        {
            if (e.type != SType::Str || !e.str || !v) return false;
            size_t n = 0;
            for (; v[n]; ++n)
                if (n >= e.str->maxLen || (unsigned char)v[n] < 0x20 || (unsigned char)v[n] > 0x7E) return false;
            return true;
        }

        bool setText(Model& m, const SEntry& e, const char* v, uint8_t* apply)
        {
            if ((e.flags & F_RO) || !available(m, e) || !validText(e, v)) return false;
            char was[64], now[64];
            getText(m, e, was, sizeof(was));
            e.str->put(m, v);
            getText(m, e, now, sizeof(now));
            if (apply && strcmp(was, now)) *apply |= e.apply;
            return true;
        }

        const char* decText(char* out, size_t cap, int32_t v, uint8_t d)
        {
            if (!d) { snprintf(out, cap, "%ld", (long)v); return out; }
            int32_t p = 1;
            for (uint8_t i = 0; i < d; ++i) p *= 10;
            const uint32_t a = v < 0 ? (uint32_t)-(int64_t)v : (uint32_t)v;
            snprintf(out, cap, "%s%lu.%0*lu", v < 0 ? "-" : "", (unsigned long)(a / p), (int)d,
                     (unsigned long)(a % p));
            return out;
        }

        bool decParse(double x, uint8_t d, int32_t& out)
        {
            for (uint8_t i = 0; i < d; ++i) x *= 10;
            if (!isfinite(x) || x > INT32_MAX || x < INT32_MIN) return false;
            out = (int32_t)lround(x);
            return true;
        }
    }
}
