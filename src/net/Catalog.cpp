#include "Catalog.h"
#include <stdio.h>
#include <string.h>
#include "../Features.h"
#include "../utils/UiList.h"
#include "../utils/Log.h"
#include "../keys/KeyAction.h"
#include "../keys/KeyDests.h"
#include "../keys/MacroEngine.h"
#include "../knobs/KnobConfig.h"
#include "../cfg/SettingsRegistry.h"
#include "../media/MediaRouter.h"
#include "../capture/CapFormat.h"
#include "../bus/Autobaud.h"
#include "../proto/canbox/Rav4Profile.h"
#include "../proto/canbox/Rav4Catalog.h"
#include "../proto/canbox/Rav4Emitter.h"
#include "../proto/canbox/RaiseTargets.h"
#include "../vehicle/DecodeTable.h"
#if FEATURE_DISPLAY_CAN
#include <AffaDisplay.h>
#include "../display/MediaScreen.h"
#include "../display/MediaPane.h"
#include "../keys/AffaMap.h"
#endif

namespace Catalog
{
    // ---- the lists with no other owner ----------------------------------------

    // Config.h bleMode; `ams`: AMS, ANCS and CTS run in this mode.
    struct BleMode { const char* id; const char* name; bool ams; };
    static const BleMode kBleModes[] = {
        {"off",      "Вимкнено",                        false},
        {"ams",      "iPhone: музика, сповіщення, час", true},
        {"keyboard", "Пульт HID",                       false},
        {"both",     "iPhone + пульт HID",              true},
    };

    bool bleModeKnown(const char* id)
    {
        if (id) for (const auto& m : kBleModes) if (!strcmp(id, m.id)) return true;
        return false;
    }

    uint8_t bleModeCount() { return sizeof(kBleModes) / sizeof(kBleModes[0]); }
    const char* bleModeId(uint8_t i)   { return kBleModes[i < bleModeCount() ? i : 0].id; }
    const char* bleModeName(uint8_t i) { return kBleModes[i < bleModeCount() ? i : 0].name; }

#if FEATURE_DISPLAY_CAN
    // The ids AffaPort::createDisplay() builds and NVS displayType stores.
    // caps mirror each panel's supports(); a layout is offered on a panel
    // whose caps name it.
    struct Panel { const char* id; const char* name; const char* const* caps; uint8_t nCaps; };
    static const char* const kCarminatCaps[] = {"text", "menu", "info", "fullscreen", "popup", "pane"};
    static const char* const kUpdateListCaps[] = {"text"};
    static const Panel kPanels[] = {
#if AFFA_PANEL_CARMINAT
        {"carminat",        "Carminat",        kCarminatCaps,   6},
#endif
#if AFFA_PANEL_UPDATELIST
        {"updatelist",      "Update List",     kUpdateListCaps, 1},
#endif
    };

    bool panelKnown(const char* id)
    {
        if (id) for (const auto& p : kPanels) if (!strcmp(id, p.id)) return true;
        return false;
    }

    uint8_t panelCount() { return sizeof(kPanels) / sizeof(kPanels[0]); }
    const char* panelId(uint8_t i)   { return i < panelCount() ? kPanels[i].id : ""; }
    const char* panelName(uint8_t i) { return i < panelCount() ? kPanels[i].name : ""; }

    // The menu's scroll-arrow byte, from the library's captured constants.
    struct Arrow { uint8_t v; const char* name; bool up, down; };
    static const Arrow kArrows[] = {
        {affa::carminat::kScrollNone, "без стрілок", false, false},
        {affa::carminat::kScrollUp,   "▲",           true,  false},
        {affa::carminat::kScrollDown, "▼",           false, true},
        {affa::carminat::kScrollBoth, "▲ і ▼",       true,  true},
    };

    bool arrowsKnown(int v)
    {
        for (const auto& a : kArrows) if (v == a.v) return true;
        return false;
    }

    // The service console: one row per /api/display/<cmd> route in WebUi.cpp
    // (tools/check_nav.py checks each cmd has one). Fields are its parameters.
    struct Command { const char* cmd; const char* name; const UiField* fields; uint8_t nFields; };
    static const UiField kText[]      = {uiText("text", "текст", 14)};
    static const UiField kMenu[]      = {uiText("header", "заголовок", 26), uiText("row0", "рядок 1", 26),
                                         uiText("row1", "рядок 2", 26), uiEnum("scroll", "стрілки", "arrows")};
    static const UiField kHighlight[] = {uiNum("row", "рядок", 0, 1)};
    static const UiField kInfo[]      = {uiText("l1", "рядок 1", 8), uiText("l2", "рядок 2", 8),
                                         uiText("l3", "рядок 3", 8)};
    static const UiField kFull[]      = {uiText("l1", "рядок 1", 26), uiText("l2", "рядок 2", 26),
                                         uiText("l3", "рядок 3", 26)};
    static const UiField kPopup[]     = {uiText("text", "текст", 16), uiEnum("icon", "значки", "icons"),
                                         uiNum("ms", "зникне через, мс (0 = висить)", 0, 60000)};
    static const UiField kStyled[]    = {uiText("text", "текст", 14), uiEnum("icon", "значки", "icons"),
                                         uiEnum("src", "мітка", "src"), uiEnum("fmt", "формат", "fmt")};
    static const UiField kConfirm[]   = {uiText("caption", "кнопка", 6), uiText("row0", "рядок 1", 26),
                                         uiText("row1", "рядок 2", 26)};
    static const UiField kMsgbox[]    = {uiText("row0", "рядок 1", 26), uiText("row1", "рядок 2", 26),
                                         uiText("labels", "кнопки через кому", 13),
                                         uiNum("buttons", "кнопок", 0, 2), uiNum("selected", "вибрана", 0, 1)};
    static const UiField kSelect[]    = {uiNum("index", "кнопка", 0, 1)};
    static const UiField kNotify[]    = {uiText("title", "заголовок", 26), uiText("body", "текст", 52)};
    static const UiField kClock[]     = {uiText("time", "ГГХХ", 4)};
    static const UiField kRelease[]   = {uiNum("ms", "мс (0 = доки не скасуєш)", 0, 60000)};
    #define FLDS(a) a, (uint8_t)(sizeof(a) / sizeof(a[0]))
    static const Command kConsole[] = {
        {"text",         "Текст",                   FLDS(kText)},
        {"menu",         "Меню",                    FLDS(kMenu)},
        {"highlight",    "Виділити рядок меню",     FLDS(kHighlight)},
        {"info",         "Інфо 3×8",                FLDS(kInfo)},
        {"fullscreen",   "Повний екран",            FLDS(kFull)},
        {"popup",        "Попап",                   FLDS(kPopup)},
        {"styled",       "Текст зі значками",       FLDS(kStyled)},
        {"confirm",      "Так/Ні",                  FLDS(kConfirm)},
        {"msgbox",       "Вікно з кнопками",        FLDS(kMsgbox)},
        {"selectbutton", "Перейти на кнопку вікна", FLDS(kSelect)},
        {"notify",       "Сповіщення",              FLDS(kNotify)},
        {"clock",        "Годинник",                FLDS(kClock)},
        {"release",      "Повернути медіа через",   FLDS(kRelease)},
        {"clear",        "Повернути медіа зараз",   nullptr, 0},
        {"abort",        "Очистити чергу",          nullptr, 0},
    };
    #undef FLDS
#else
    uint8_t panelCount() { return 0; }
    const char* panelId(uint8_t)   { return ""; }
    const char* panelName(uint8_t) { return ""; }
#endif

    // ---- what this image has (Catalog.h, Feature) --------------------------------
    // The same switches the routes are gated by: display = /api/display, media,
    // affamap; ble (with display) = /api/ble; ancs = /api/notifications;
    // vehicle = /api/can*; can2 = /api/can2; capture = /api/cap*;
    // knobs = /api/knobs*; matrix = /api/keymap*; res = /api/res*; logfs =
    // /api/logs/file; usb and uart = routing destinations.
    // Macros, routing, the map and the Raise console are in every image.
    #define FEATURE_ROW(id, m) {id, #m, (m) != 0}
    const Feature FEATURES[] = {
        FEATURE_ROW("display", FEATURE_DISPLAY_CAN),
        FEATURE_ROW("ble",     FEATURE_BLE),
        FEATURE_ROW("ancs",    FEATURE_ANCS),
        FEATURE_ROW("vehicle", FEATURE_VEHICLE_CAN),
        FEATURE_ROW("can2",    FEATURE_CAN2),
        FEATURE_ROW("capture", FEATURE_CAPTURE),
        FEATURE_ROW("knobs",   FEATURE_KNOBS),
        FEATURE_ROW("matrix",  FEATURE_MATRIX),
        FEATURE_ROW("res",     FEATURE_RESISTIVE_KEYS),
        FEATURE_ROW("logfs",   FEATURE_LOGFS),
        FEATURE_ROW("usb",     FEATURE_USB_HID),
        FEATURE_ROW("uart",    FEATURE_CANBOX_UART),
    };
    #undef FEATURE_ROW
    const size_t FEATURE_COUNT = sizeof(FEATURES) / sizeof(FEATURES[0]);

    // ---- the writer -------------------------------------------------------------

    namespace
    {
        // Streams JSON with the commas placed for it: a value or key after
        // another one at the same level gets a separator, the first does not.
        class J {
        public:
            explicit J(Out& o) : _o(o) {}

            J& obj() { sep(); put('{'); _first = true; return *this; }
            J& arr() { sep(); put('['); _first = true; return *this; }
            J& endObj() { put('}'); _first = false; return *this; }
            J& endArr() { put(']'); _first = false; return *this; }

            J& k(const char* key) { sep(); quoted(key); put(':'); _first = true; return *this; }
            J& s(const char* v) { sep(); if (v) quoted(v); else raw("null"); _first = false; return *this; }
            J& n(long v) { char b[16]; snprintf(b, sizeof(b), "%ld", v); sep(); raw(b); _first = false; return *this; }
            J& b(bool v) { sep(); raw(v ? "true" : "false"); _first = false; return *this; }
            J& null() { sep(); raw("null"); _first = false; return *this; }
            // A number already formatted (a decimal: "655.35").
            J& lit(const char* v) { sep(); raw(v); _first = false; return *this; }

            J& ks(const char* key, const char* v) { return k(key).s(v); }
            J& kn(const char* key, long v) { return k(key).n(v); }
            J& kb(const char* key, bool v) { return k(key).b(v); }

        private:
            void put(char c) { _o.put(&c, 1); }
            void raw(const char* s) { _o.put(s, strlen(s)); }
            void sep() { if (!_first) put(','); }
            void quoted(const char* s)
            {
                put('"');
                for (; *s; ++s) {
                    if (*s == '"' || *s == '\\') put('\\');
                    put(*s);
                }
                put('"');
            }

            Out& _o;
            bool _first = true;
        };

        // "0x55": the console posts these as they are and the firmware parses base 0.
        const char* hex(char* b, size_t cap, unsigned v, int digits)
        {
            snprintf(b, cap, "0x%0*X", digits, v);
            return b;
        }

        void field(J& j, const UiField& f)
        {
            j.obj().ks("n", f.n).ks("label", f.label);
            if (f.en) j.ks("enum", f.en);
            else if (f.num) {
                j.kb("num", true).kn("min", f.min).kn("max", f.max);
                if (f.step) j.kn("step", f.step);
                if (f.hint) j.ks("hint", f.hint);
            } else j.kn("max", f.max);
            j.endObj();
        }

        void fields(J& j, const char* key, const UiField* f, uint8_t n)
        {
            j.k(key).arr();
            for (uint8_t i = 0; i < n; ++i) field(j, f[i]);
            j.endArr();
        }

        // The settings registry (Cfg::Registry), in table order so a group
        // comes before its children: what GET/POST /api/settings carry and how
        // the page draws each one. `menu`: also in the panel menu. `reboot`:
        // applied at the next boot. Enum rows inline, `v` = what is sent. A
        // table: `rows` and its `cols`, each like a value (id, label, type,
        // min/max or enum).
        void enumRows(J& j, const Cfg::SEnum& en)
        {
            j.k("enum").arr();
            for (uint8_t r = 0; r < en.count(); ++r)
                j.obj().ks("v", en.id(r)).ks("name", en.name(r)).endObj();
            j.endArr();
        }

        void settings(J& j)
        {
            using namespace Cfg;
            static const char* const kType[] = {"group", "bool", "int", "enum", "table", "str"};
            j.k("settings").arr();
            for (size_t i = 0; i < Registry::count(); ++i) {
                const SEntry& e = Registry::at(i);
                if (!(e.flags & F_WEB)) continue;
                j.obj().ks("id", e.id).ks("label", e.label).ks("tag", e.tag)
                 .ks("type", kType[(uint8_t)e.type]);
                if (e.parent) j.ks("parent", e.parent);
                if (e.flags & F_MENU) j.kb("menu", true);
                if (e.flags & F_RO) j.kb("ro", true);
                if (e.flags & F_VOLATILE) j.kb("volatile", true);
                if (e.apply & AP_REBOOT) j.kb("reboot", true);
                if (e.type == SType::Int) {
                    j.kn("min", e.min).kn("max", e.max);
                    if (e.step > 1) j.kn("step", e.step);
                    if (e.unit) j.ks("unit", e.unit);
                }
                if (e.type == SType::Enum && e.en) enumRows(j, *e.en);
                if (e.type == SType::Str && e.str) j.kn("max", e.str->maxLen);
                if (e.type == SType::Table && e.tab) {
                    j.kn("rows", e.tab->rows()).k("cols").arr();
                    for (uint8_t c = 0; c < e.tab->cols; ++c) {
                        const SColumn& col = e.tab->col[c];
                        j.obj().ks("id", col.id).ks("label", col.label).ks("type", kType[(uint8_t)col.type]);
                        if (col.type == SType::Int) j.kn("min", col.min).kn("max", col.max);
                        if (col.type == SType::Enum && col.en) enumRows(j, *col.en);
                        j.endObj();
                    }
                    j.endArr();
                }
                j.endObj();
            }
            j.endArr();
        }

        template <size_t N>
        void names(J& j, const char* key, const UiName (&t)[N])
        {
            j.k(key).arr();
            for (const auto& r : t) j.obj().ks("v", r.id).ks("name", r.name).endObj();
            j.endArr();
        }

#if FEATURE_DISPLAY_CAN
        void layouts(J& j)
        {
            using MegaOpen::MediaScreen;
            j.k("layouts").arr();
            for (uint8_t i = 0; i < MediaScreen::LAYOUT_COUNT; ++i) {
                const auto& r = MediaScreen::layoutAt(i);
                j.obj().ks("id", r.id).ks("name", r.name)
                 .kn("cols", MediaScreen::colsFor(r.layout)).kn("rows", MediaScreen::rowsFor(r.layout))
                 .kb("pane", MediaScreen::sharesWithPane(r.layout))
                 .kb("clock", MediaScreen::panelClock(r.layout));
                fields(j, "params", r.params, r.nParams);
                j.k("panels").arr();
                for (const auto& p : kPanels)
                    for (uint8_t c = 0; c < p.nCaps; ++c)
                        if (!strcmp(p.caps[c], r.id)) j.s(p.id);
                j.endArr().endObj();
            }
            j.endArr();

            j.k("styles").arr();
            for (uint8_t i = 0; i < MediaScreen::STYLE_COUNT; ++i) {
                const auto st = (MediaScreen::Style)i;
                j.obj().ks("id", MediaScreen::styleName(st)).ks("name", MediaScreen::styleTitle(st))
                 .kb("animated", MediaScreen::styleAnimates(st)).endObj();
            }
            j.endArr();

            using MegaOpen::MediaPane;
            j.k("scenes").arr();
            for (uint8_t i = 0; i < MediaPane::SCENE_COUNT; ++i) {
                const auto sc = (MediaPane::Scene)i;
                j.obj().ks("id", MediaPane::sceneName(sc)).ks("name", MediaPane::sceneTitle(sc)).endObj();
            }
            j.endArr();
        }

        // The panel's header bytes, from the library's captured constants.
        void displayEnums(J& j)
        {
            namespace C = affa::carminat;
            j.k("arrows").arr();
            for (const auto& a : kArrows)
                j.obj().kn("v", a.v).ks("name", a.name).kb("up", a.up).kb("down", a.down).endObj();
            j.endArr();

            struct Byte { uint8_t v; const char* name; };
            // 0xDF and 0xFD are the captured source labels (CarminatConstants.h, kSrcIconNone);
            // 0x20 is a radio-style format from the 0x19-0x3F range (kFormatPlain).
            static const Byte kIcons[] = {{C::kIconsNone, "без значків"}, {C::kIconsAfRds, "AF-RDS"}};
            static const Byte kSrc[]   = {{C::kSrcIconNone, "нема"}, {0xDF, "MANU"}, {0xFD, "PRESET"}};
            static const Byte kFmt[]   = {{C::kFormatPlain, "звичайний текст"}, {0x20, "як радіо: 5 цифр.1"}};
            auto bytes = [&](const char* key, const Byte* t, size_t n) {
                char b[8];
                j.k(key).arr();
                for (size_t i = 0; i < n; ++i)
                    j.obj().ks("v", hex(b, sizeof(b), t[i].v, 2)).ks("name", t[i].name).endObj();
                j.endArr();
            };
            bytes("icons", kIcons, 2);
            bytes("src", kSrc, 3);
            bytes("fmt", kFmt, 2);
        }
#endif

        void enums(J& j)
        {
            j.k("enums").obj();
#if FEATURE_DISPLAY_CAN
            displayEnums(j);
#endif
            j.k("mediaSrc").arr();
            for (const auto& r : MediaRouter::SOURCES)
                j.obj().ks("v", r.id).ks("name", r.name).kb("pick", r.pick).kb("ams", r.ams).endObj();
            j.endArr();

            // 0 = autobaud; then the rates it probes, slowest first.
            uint32_t rates[Bus::Autobaud::N_RATES];
            for (uint8_t i = 0; i < Bus::Autobaud::N_RATES; ++i) rates[i] = Bus::Autobaud::rateAt(i);
            for (uint8_t i = 1; i < Bus::Autobaud::N_RATES; ++i)
                for (uint8_t k = i; k > 0 && rates[k] < rates[k - 1]; --k) {
                    const uint32_t t = rates[k]; rates[k] = rates[k - 1]; rates[k - 1] = t;
                }
            j.k("canRates").arr().obj().kn("v", 0).ks("name", "авто").endObj();
            for (uint32_t r : rates) {
                char b[8];
                if (r % 1000000 == 0) snprintf(b, sizeof(b), "%luM", (unsigned long)(r / 1000000));
                else snprintf(b, sizeof(b), "%luk", (unsigned long)(r / 1000));
                j.obj().kn("v", (long)r).ks("name", b).endObj();
            }
            j.endArr();

            names(j, "capModes", CapFormat::MODE_NAMES);

            auto units = [&](const char* key, const Canbox::UnitRow* t, size_t n) {
                j.k(key).arr();
                for (size_t i = 0; i < n; ++i) j.obj().kn("v", t[i].v).ks("name", t[i].name).endObj();
                j.endArr();
            };
            units("fuelUnits", Canbox::FUEL_UNITS, sizeof(Canbox::FUEL_UNITS) / sizeof(Canbox::FUEL_UNITS[0]));
            units("tripUnits", Canbox::TRIP_UNITS, sizeof(Canbox::TRIP_UNITS) / sizeof(Canbox::TRIP_UNITS[0]));

            names(j, "mapSrc", Vehicle::MAP_SRC);

            j.k("macroTriggers").arr();
            for (const auto& t : Keys::TRIGGERS)
                j.obj().ks("v", t.id).ks("name", t.name).kb("taps", t.taps).endObj();
            j.endArr();
            j.k("macroEffects").arr();
            for (const auto& e : Keys::EFFECTS)
                j.obj().ks("v", e.id).ks("name", e.name).ks("field", e.field).endObj();
            j.endArr();

            names(j, "ledOrder", Knobs::ORDER_NAMES);
            names(j, "ledIdle", Knobs::ANIM_NAMES);
            names(j, "ledReact", Knobs::REACT_NAMES);

            j.k("logLevels").arr();
            for (uint8_t i = 0; i < Log::LEVELS; ++i)
                j.obj().kn("v", i).ks("name", Log::LEVEL_NAMES[i]).endObj();
            j.endArr();
            j.endObj();
        }

        void keys(J& j)
        {
            using namespace Keys;
            j.k("classes").arr();
            for (uint8_t i = 0; i < CLASS_BITS; ++i)
                j.obj().kn("bit", i).ks("name", classTitle((uint8_t)(1u << i))).endObj();
            j.endArr();

            // Every sink the routing matrix has; `built` false where this image
            // compiled it out, so the page greys it instead of hiding it.
            j.k("dests").arr();
            j.obj().ks("id", DEST_IDS[DEST_UART]).ks("name", "ГУ").kb("built", FEATURE_CANBOX_UART).endObj();
            j.obj().ks("id", DEST_IDS[DEST_AMS]).ks("name", "iPhone").kb("built", FEATURE_BLE).endObj();
            j.obj().ks("id", DEST_IDS[DEST_HID]).ks("name", "HID").kb("built", FEATURE_BLE).endObj();
            j.obj().ks("id", DEST_IDS[DEST_RES]).ks("name", "Резистор").kb("built", FEATURE_RESISTIVE_KEYS).endObj();
#if FEATURE_USB_HID
            j.obj().ks("id", DEST_IDS[DEST_USB]).ks("name", "USB").kb("built", true).endObj();
#endif
            j.endArr();

            // The encoder phases are matrix positions, never a choice.
            j.k("actions").arr();
            for (uint8_t i = 0; i < (uint8_t)Action::_Count; ++i) {
                const auto a = (Action)i;
                if (isEncoderPhase(a)) continue;
                j.obj().ks("id", name(a)).ks("name", title(a)).k("class");
                const uint8_t c = classOf(a);
                int bit = -1;
                for (uint8_t b = 0; b < CLASS_BITS; ++b) if (c == (1u << b)) bit = b;
                if (bit < 0) j.null(); else j.n(bit);
                char h[4] = "";
                if (const uint8_t code = Canbox::rav4CodeFor(a)) snprintf(h, sizeof(h), "%02X", code);
                j.ks("hu", h).endObj();
            }
            j.endArr();

            j.k("panelKeys").arr();
#if FEATURE_DISPLAY_CAN
            for (uint8_t i = 0; i < AffaMap::KEYS; ++i) {
                char w[8];
                j.obj().ks("id", AffaMap::keyName(i)).ks("name", AffaMap::keyTitle(i))
                 .ks("wire", hex(w, sizeof(w), (unsigned)AffaMap::keyAt(i), 4)).ks("src", "wheel");
                // Only where it is false: the CAN B sender cannot send it held.
                if (!AffaMap::wireHold(AffaMap::keyAt(i))) j.kb("wireHold", false);
                j.endObj();
            }
#endif
            for (const auto& k : Knobs::KNOB_KEYS)
                j.obj().ks("id", k.id).ks("name", k.name).ks("wire", "").ks("src", "knob").endObj();
            j.endArr();

            j.k("huKeys").arr();
            for (uint8_t i = 0; i < Canbox::keyCodeCount(); ++i) {
                const auto& kc = Canbox::keyCodeAt(i);
                char h[4];
                snprintf(h, sizeof(h), "%02X", kc.code);
                j.obj().ks("code", h).ks("name", kc.name).endObj();
            }
            j.endArr();
        }
    }

    void write(Out& out)
    {
        J j(out);
        j.obj().kn("v", 1);
        j.k("features").obj();
        for (const auto& f : FEATURES) j.kb(f.id, f.on);
        j.endObj();
#if FEATURE_DISPLAY_CAN
        layouts(j);
#else
        j.k("layouts").arr().endArr().k("styles").arr().endArr().k("scenes").arr().endArr();
#endif
        enums(j);

        j.k("panels").arr();
#if FEATURE_DISPLAY_CAN
        for (const auto& p : kPanels) {
            j.obj().ks("id", p.id).ks("name", p.name).k("caps").arr();
            for (uint8_t c = 0; c < p.nCaps; ++c) j.s(p.caps[c]);
            j.endArr().endObj();
        }
#endif
        j.endArr();

        j.k("bleModes").arr();
        for (const auto& m : kBleModes) j.obj().ks("id", m.id).ks("name", m.name).kb("ams", m.ams).endObj();
        j.endArr();

        settings(j);
        keys(j);

        j.k("console").arr();
#if FEATURE_DISPLAY_CAN
        for (const auto& c : kConsole) {
            j.obj().ks("cmd", c.cmd).ks("name", c.name);
            fields(j, "fields", c.fields, c.nFields);
            j.endObj();
        }
#endif
        j.endArr();

        // Every field the RAV4 profile carries (RaiseTargets): min and max with
        // the decimal point placed, as the page types and shows values.
        j.k("raise").arr();
        for (uint8_t i = 0; i < Canbox::RaiseTargets::count(); ++i) {
            const Canbox::TargetView t = Canbox::RaiseTargets::at(i);
            char lo[16], hi[16];
            j.obj().ks("id", t.id).ks("group", t.group).ks("name", t.name).ks("unit", t.unit)
                .ks("cmd", t.cmd).ks("kind", t.boolean ? "b" : "n")
                .k("min").lit(Canbox::RaiseTargets::fmt(lo, sizeof(lo), t.min, t.dec))
                .k("max").lit(Canbox::RaiseTargets::fmt(hi, sizeof(hi), t.max, t.dec))
                .kn("dec", t.dec).kb("fw", t.fw).endObj();
        }
        j.endArr();

        j.k("logTags").arr();
        for (const auto& t : Log::TAG_ROWS) j.s(t.tag);
        j.endArr();
        j.endObj();
    }
}
