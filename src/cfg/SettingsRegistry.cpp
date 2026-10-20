#include "SettingsRegistry.h"
#include <stdio.h>
#include <string.h>
#include "../display/MediaScreen.h"
#include "../display/MediaPane.h"
#include "../media/MediaRouter.h"
#include "../bus/Autobaud.h"
#include "../net/Catalog.h"

namespace Cfg
{
    namespace
    {
        using MegaOpen::MediaScreen;
        using MegaOpen::MediaPane;

        // ---- enum rows -----------------------------------------------------

        // Layouts, in MediaScreen's own row order.
        const SEnum kLayouts = {
            [] { return MediaScreen::LAYOUT_COUNT; },
            [](uint8_t i) { return MediaScreen::layoutAt(i).id; },
            [](uint8_t i) { return MediaScreen::layoutAt(i).name; },
        };

        const SEnum kStyles = {
            [] { return MediaScreen::STYLE_COUNT; },
            [](uint8_t i) { return MediaScreen::styleName((MediaScreen::Style)i); },
            [](uint8_t i) { return MediaScreen::styleTitle((MediaScreen::Style)i); },
        };

        // mediaPane: the scenes, then "auto" (the carousel) as the last row,
        // so scene i stays row i.
        const SEnum kPanes = {
            [] { return (uint8_t)(MediaPane::SCENE_COUNT + 1); },
            [](uint8_t i) {
                return i < MediaPane::SCENE_COUNT ? MediaPane::sceneName((MediaPane::Scene)i) : "auto";
            },
            [](uint8_t i) {
                return i < MediaPane::SCENE_COUNT ? MediaPane::sceneTitle((MediaPane::Scene)i) : "карусель";
            },
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
        };

        const SEnum kBleModes = { Catalog::bleModeCount, Catalog::bleModeId, Catalog::bleModeName };
        const SEnum kPanels   = { Catalog::panelCount,   Catalog::panelId,   Catalog::panelName };

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
            [](uint8_t i) { return i ? rateId(i) : "авто"; },
        };

        // Knob colours: a whole zone in one colour, the menu's way to pick one.
        // Row 0 is whatever the web set LED by LED; it is shown, and choosing
        // it changes nothing.
        struct Preset { const char* id; const char* name; Knobs::Rgb c; };
        const Preset kPresets[] = {
            {"custom", "свій",        {0, 0, 0}},
            {"white",  "білий",       {255, 255, 255}},
            {"red",    "червоний",    {255, 0, 0}},
            {"orange", "помаранч.",   {255, 80, 0}},
            {"amber",  "бурштин",     {255, 140, 0}},
            {"yellow", "жовтий",      {255, 220, 0}},
            {"green",  "зелений",     {0, 255, 0}},
            {"cyan",   "бірюзовий",   {0, 255, 255}},
            {"blue",   "синій",       {0, 0, 255}},
            {"purple", "фіолетовий",  {160, 0, 255}},
            {"pink",   "рожевий",     {255, 0, 120}},
        };
        constexpr uint8_t PRESETS = sizeof(kPresets) / sizeof(kPresets[0]);
        const SEnum kColors = {
            [] { return PRESETS; },
            [](uint8_t i) { return kPresets[i < PRESETS ? i : 0].id; },
            [](uint8_t i) { return kPresets[i < PRESETS ? i : 0].name; },
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

#define UI_NAMES_ENUM(T)                                          \
        SEnum{ [] { return (uint8_t)(sizeof(T) / sizeof(T[0])); }, \
               [](uint8_t i) { return T[i].id; },                  \
               [](uint8_t i) { return T[i].name; } }
        const SEnum kReact = UI_NAMES_ENUM(Knobs::REACT_NAMES);
        const SEnum kAnim  = UI_NAMES_ENUM(Knobs::ANIM_NAMES);
#undef UI_NAMES_ENUM

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
        constexpr SEntry G(const char* id, const char* label, const char* tag,
                           const char* parent, uint8_t flags)
        {
            return SEntry{id, label, tag, parent, SType::Group, flags, AP_NONE,
                          0, 0, 0, nullptr, nullptr, nullptr, nullptr};
        }

        // Names: the Settings field `f` via s, a knobs field via k.
#define S_GET(expr) [](const Model& m) -> int32_t { const Settings& s = *m.s; return (int32_t)(expr); }
#define S_PUT(stmt) [](Model& m, int32_t v) { Settings& s = *m.s; (void)v; stmt; }
#define K_GET(expr) [](const Model& m) -> int32_t { const Knobs::Config& k = *m.k; return (int32_t)(expr); }
#define K_PUT(stmt) [](Model& m, int32_t v) { Knobs::Config& k = *m.k; (void)v; stmt; }

        const SEntry kTable[] = {
            // ---- the screen ------------------------------------------------
            G("display", "Екран", "Display", nullptr, WM),
            {"mediaScreen", "Екран «зараз грає»", "Audio screen", "display", SType::Bool, WM, AP_UI,
             0, 1, 0, nullptr, nullptr, S_GET(s.mediaScreen), S_PUT(s.mediaScreen = v != 0)},
            {"mediaLayout", "Розкладка", "Layout", "display", SType::Enum, WM, AP_UI,
             0, 0, 0, nullptr, &kLayouts, S_GET(rowOf(kLayouts, s.mediaLayout)),
             S_PUT(s.mediaLayout = kLayouts.id((uint8_t)v))},
            {"mediaStyle", "Стиль тексту", "Text style", "display", SType::Enum, WM, AP_UI,
             0, 0, 0, nullptr, &kStyles, S_GET(rowOf(kStyles, s.mediaStyle)),
             S_PUT(s.mediaStyle = kStyles.id((uint8_t)v))},
            {"mediaPane", "Картинка", "Scene", "display", SType::Enum, WM, AP_UI,
             0, 0, 0, nullptr, &kPanes, S_GET(rowOf(kPanes, s.mediaPane)),
             S_PUT(s.mediaPane = kPanes.id((uint8_t)v))},
            // 5..600 s per stop (the /api/media rule).
            {"paneCycleS", "Карусель, с", "Carousel", "display", SType::Int, WM, AP_UI,
             5, 600, 5, "s", nullptr, S_GET(s.paneCycleS), S_PUT(s.paneCycleS = (uint16_t)v)},
            // 1..6: past that it stops reading as motion at two images a second.
            {"paneSpeed", "Швидкість картинки", "Scene speed", "display", SType::Int, WM, AP_UI,
             1, 6, 1, nullptr, nullptr, S_GET(s.paneSpeed), S_PUT(s.paneSpeed = (uint8_t)v)},
            // 0 = off, else 1..20 s; a value under a second becomes one.
            {"trackSplashMs", "Заставка треку, мс", "Track splash", "display", SType::Int, WM, AP_UI,
             0, 20000, 1000, "ms", nullptr, S_GET(s.trackSplashMs),
             S_PUT(s.trackSplashMs = (uint16_t)(v <= 0 ? 0 : (v < 1000 ? 1000 : v)))},
            {"autoTime", "Час з телефону", "Phone clock", "display", SType::Bool, WM, AP_UI,
             0, 1, 0, nullptr, nullptr, S_GET(s.autoTime), S_PUT(s.autoTime = v != 0)},
            {"keyPopup", "Показувати натискання", "Key popup", "display", SType::Bool, WM, AP_UI,
             0, 1, 0, nullptr, nullptr, S_GET(s.keyPopup), S_PUT(s.keyPopup = v != 0)},
            // Shorter than a glance, or longer than a screen, is refused.
            {"keyPopupMs", "Натискання, мс", "Popup time", "display", SType::Int, WM, AP_UI,
             300, 10000, 100, "ms", nullptr, S_GET(s.keyPopupMs), S_PUT(s.keyPopupMs = (uint16_t)v)},
            {"ancsPopup", "Сповіщення iPhone", "Notifications", "display", SType::Bool, WM, AP_UI,
             0, 1, 0, nullptr, nullptr, S_GET(s.ancsPopup), S_PUT(s.ancsPopup = v != 0)},
            {"mediaSource", "Джерело назви треку", "Track source", "display", SType::Enum, WM, AP_MEDIA,
             0, 0, 0, nullptr, &kSources, S_GET(rowOf(kSources, s.mediaSource)),
             S_PUT(s.mediaSource = kSources.id((uint8_t)v))},

            // ---- the knob lights -------------------------------------------
            G("lights", "Підсвітка", "Lights", nullptr, WM),
            {"ledEnabled", "Підсвітка", "Lights on", "lights", SType::Bool, WM, AP_KNOBS,
             0, 1, 0, nullptr, nullptr, K_GET(k.ledEnabled), K_PUT(k.ledEnabled = v != 0)},
            {"ledBrightness", "Яскравість", "Brightness", "lights", SType::Int, WM, AP_KNOBS,
             0, 255, 15, nullptr, nullptr, K_GET(k.brightness), K_PUT(k.brightness = (uint8_t)v)},
            G("lights.r", "Правий енкодер", "Right knob", "lights", WM),
            {"ledR.react", "Реакція", "Reaction", "lights.r", SType::Enum, WM, AP_KNOBS,
             0, 0, 0, nullptr, &kReact, K_GET(k.zone[Knobs::ZONE_RIGHT].react),
             K_PUT(k.zone[Knobs::ZONE_RIGHT].react = (Knobs::React)v)},
            {"ledR.anim", "У спокої", "Idle", "lights.r", SType::Enum, WM, AP_KNOBS,
             0, 0, 0, nullptr, &kAnim, K_GET(k.zone[Knobs::ZONE_RIGHT].anim),
             K_PUT(k.zone[Knobs::ZONE_RIGHT].anim = (Knobs::Anim)v)},
            {"ledR.color", "Колір", "Colour", "lights.r", SType::Enum, WM, AP_KNOBS,
             0, 0, 0, nullptr, &kColors, K_GET(zoneColor(k.zone[Knobs::ZONE_RIGHT])),
             K_PUT(setZoneColor(k.zone[Knobs::ZONE_RIGHT], v))},
            G("lights.l", "Лівий енкодер", "Left knob", "lights", WM),
            {"ledL.react", "Реакція", "Reaction", "lights.l", SType::Enum, WM, AP_KNOBS,
             0, 0, 0, nullptr, &kReact, K_GET(k.zone[Knobs::ZONE_LEFT].react),
             K_PUT(k.zone[Knobs::ZONE_LEFT].react = (Knobs::React)v)},
            {"ledL.anim", "У спокої", "Idle", "lights.l", SType::Enum, WM, AP_KNOBS,
             0, 0, 0, nullptr, &kAnim, K_GET(k.zone[Knobs::ZONE_LEFT].anim),
             K_PUT(k.zone[Knobs::ZONE_LEFT].anim = (Knobs::Anim)v)},
            {"ledL.color", "Колір", "Colour", "lights.l", SType::Enum, WM, AP_KNOBS,
             0, 0, 0, nullptr, &kColors, K_GET(zoneColor(k.zone[Knobs::ZONE_LEFT])),
             K_PUT(setZoneColor(k.zone[Knobs::ZONE_LEFT], v))},

            // ---- keys and the head-unit link (web only) ----------------------
            // The timing ranges are wide on purpose: the debouncer had no bounds
            // before, so they only stop nonsense.
            G("keys", "Клавіші", "Keys", nullptr, W),
            {"debounceMs", "Брязкіт, мс", "Debounce", "keys", SType::Int, W, AP_KEYS,
             1, 200, 1, "ms", nullptr, S_GET(s.timing.debounceMs), S_PUT(s.timing.debounceMs = (uint16_t)v)},
            {"fastDebounceMs", "Швидкий брязкіт, мс", "Fast debounce", "keys", SType::Int, W, AP_KEYS,
             0, 50, 1, "ms", nullptr, S_GET(s.timing.fastDebounceMs), S_PUT(s.timing.fastDebounceMs = (uint16_t)v)},
            {"longMs", "Довге натискання, мс", "Long press", "keys", SType::Int, W, AP_KEYS,
             200, 5000, 50, "ms", nullptr, S_GET(s.timing.longMs), S_PUT(s.timing.longMs = (uint16_t)v)},
            {"repeatMs", "Повтор, мс", "Repeat", "keys", SType::Int, W, AP_KEYS,
             50, 2000, 10, "ms", nullptr, S_GET(s.timing.repeatMs), S_PUT(s.timing.repeatMs = (uint16_t)v)},
            {"repeatEnabled", "Повтор утриманням", "Repeat on", "keys", SType::Bool, W, AP_KEYS,
             0, 1, 0, nullptr, nullptr, S_GET(s.timing.repeatEnabled), S_PUT(s.timing.repeatEnabled = v != 0)},
            {"encInverted", "Енкодер навпаки", "Enc reversed", "keys", SType::Bool, W, AP_KEYS,
             0, 1, 0, nullptr, nullptr, S_GET(s.encInverted), S_PUT(s.encInverted = v != 0)},
            // Destination class masks (Keys::CLASS_*).
            {"routeUart", "Клавіші в ГУ (UART)", "Route UART", "keys", SType::Int, W, AP_KEYS,
             0, Keys::CLASS_ALL, 1, nullptr, nullptr, S_GET(s.routeUart), S_PUT(s.routeUart = (uint8_t)v)},
            {"routeAms", "Клавіші в iPhone", "Route AMS", "keys", SType::Int, W, AP_KEYS,
             0, Keys::CLASS_ALL, 1, nullptr, nullptr, S_GET(s.routeAms), S_PUT(s.routeAms = (uint8_t)v)},
            {"routeHid", "Клавіші в HID", "Route HID", "keys", SType::Int, W, AP_KEYS,
             0, Keys::CLASS_ALL, 1, nullptr, nullptr, S_GET(s.routeHid), S_PUT(s.routeHid = (uint8_t)v)},
            {"routeRes", "Клавіші в KEY1/KEY2", "Route RES", "keys", SType::Int, W, AP_KEYS,
             0, Keys::CLASS_ALL, 1, nullptr, nullptr, S_GET(s.routeRes), S_PUT(s.routeRes = (uint8_t)v)},
            {"routeUsb", "Клавіші в USB", "Route USB", "keys", SType::Int, W, AP_KEYS,
             0, Keys::CLASS_ALL, 1, nullptr, nullptr, S_GET(s.routeUsb), S_PUT(s.routeUsb = (uint8_t)v)},

            G("link", "Звʼязок з ГУ", "Head unit", nullptr, W),
            {"uartEnabled", "ГУ по UART", "UART on", "link", SType::Bool, W, AP_KEYS,
             0, 1, 0, nullptr, nullptr, S_GET(s.uartEnabled), S_PUT(s.uartEnabled = v != 0)},
            {"pulseMs", "Довжина натиску в ГУ, мс", "Key pulse", "link", SType::Int, W, AP_KEYS,
             40, 500, 10, "ms", nullptr, S_GET(s.pulseMs), S_PUT(s.pulseMs = (uint16_t)v)},
            // Fixed by the canbox profile; shown, not set.
            {"uartBaud", "Швидкість UART", "UART baud", "link", SType::Int, W | F_RO, AP_REBOOT,
             1200, 921600, 0, nullptr, nullptr, S_GET(s.uartBaud), S_PUT(s.uartBaud = (uint32_t)v)},
            {"resEnabled", "KEY1/KEY2", "KEY1/KEY2", "link", SType::Bool, W, AP_KEYS,
             0, 1, 0, nullptr, nullptr, S_GET(s.resEnabled), S_PUT(s.resEnabled = v != 0)},
            // 0 = learn it (HuMediaSource).
            {"huTitleCmd", "Команда назви", "HU title cmd", "link", SType::Int, W, AP_MEDIA,
             0, 255, 1, nullptr, nullptr, S_GET(s.huTitleCmd), S_PUT(s.huTitleCmd = (uint8_t)v)},
            {"huArtistCmd", "Команда виконавця", "HU artist cmd", "link", SType::Int, W, AP_MEDIA,
             0, 255, 1, nullptr, nullptr, S_GET(s.huArtistCmd), S_PUT(s.huArtistCmd = (uint8_t)v)},
            {"huTitleFirst", "Назва першою", "Title first", "link", SType::Bool, W, AP_MEDIA,
             0, 1, 0, nullptr, nullptr, S_GET(s.huTitleFirst), S_PUT(s.huTitleFirst = v != 0)},

            // ---- read at boot ----------------------------------------------
            G("system", "Система", "System", nullptr, W),
            {"bleMode", "Bluetooth", "Bluetooth", "system", SType::Enum, W, AP_REBOOT,
             0, 0, 0, nullptr, &kBleModes, S_GET(rowOf(kBleModes, s.bleMode)),
             S_PUT(s.bleMode = kBleModes.id((uint8_t)v))},
            {"displayType", "Тип щитка", "Panel", "system", SType::Enum, W, AP_REBOOT,
             0, 0, 0, nullptr, &kPanels, S_GET(rowOf(kPanels, s.displayType)),
             S_PUT(s.displayType = kPanels.id((uint8_t)v))},
            {"funcReg", "Реєструвати щиток", "FuncsReg", "system", SType::Bool, W, AP_REBOOT,
             0, 1, 0, nullptr, nullptr, S_GET(s.funcReg), S_PUT(s.funcReg = v != 0)},
            {"mdnsOn", "Імʼя megaopen.local", "mDNS", "system", SType::Bool, W, AP_REBOOT,
             0, 1, 0, nullptr, nullptr, S_GET(s.mdnsOn), S_PUT(s.mdnsOn = v != 0)},
            {"can2Bitrate", "Швидкість CAN A", "CAN A rate", "system", SType::Enum, W, AP_REBOOT,
             0, 0, 0, nullptr, &kRates, S_GET(rateIndex(s.can2Bitrate)),
             S_PUT(s.can2Bitrate = rateRow((uint8_t)v))},
            // Written by net_svc when the search locks on; shown only.
            {"can2Detected", "Знайдена швидкість CAN A", "CAN A found", "system", SType::Enum, W | F_RO, AP_NONE,
             0, 0, 0, nullptr, &kRates, S_GET(rateIndex(s.can2Detected)),
             S_PUT(s.can2Detected = rateRow((uint8_t)v))},
        };
#undef S_GET
#undef S_PUT
#undef K_GET
#undef K_PUT

        constexpr size_t N = sizeof(kTable) / sizeof(kTable[0]);
    }

    namespace Registry
    {
        size_t count() { return N; }
        const SEntry& at(size_t i) { return kTable[i < N ? i : 0]; }

        int indexOf(const char* id)
        {
            if (id) for (size_t i = 0; i < N; ++i) if (!strcmp(id, kTable[i].id)) return (int)i;
            return -1;
        }

        const SEntry* find(const char* id)
        {
            const int i = indexOf(id);
            return i < 0 ? nullptr : &kTable[i];
        }

        bool available(const Model& m, const SEntry& e)
        {
            if (e.type == SType::Group) return true;
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
            return e.type != SType::Group && v >= minOf(e) && v <= maxOf(e);
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
    }
}
