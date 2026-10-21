#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "net/Catalog.h"

// /api/catalog is the page's only source for its lists, so a malformed or
// inconsistent catalog is a blank screen on the board, not a wrong label. These
// tests parse what Catalog::write() produces (the board streams the very same
// bytes) and check what the page relies on: the keys exist, ids are unique,
// every field's `enum` names a list that is there.
//
// They also WRITE build/catalog.json: tools/build_sim.py embeds it, so the
// simulator serves the firmware's catalog instead of a hand-kept copy.

void setUp() {}
void tearDown() {}

// ---- a minimal JSON reader: enough to hold the catalog, strict on syntax -----

struct V {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<std::shared_ptr<V>> a;
    std::vector<std::pair<std::string, std::shared_ptr<V>>> o;

    const V* get(const char* k) const {
        for (const auto& kv : o) if (kv.first == k) return kv.second.get();
        return nullptr;
    }
};

struct Reader {
    const char* p;
    bool ok = true;

    void ws() { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p; }
    bool eat(char c) { ws(); if (*p == c) { ++p; return true; } return false; }
    void fail() { ok = false; }

    std::string str() {
        std::string out;
        if (!eat('"')) { fail(); return out; }
        while (*p && *p != '"') {
            if ((unsigned char)*p < 0x20) { fail(); return out; }
            if (*p == '\\') {
                ++p;
                if (*p != '"' && *p != '\\' && *p != '/' && *p != 'n' && *p != 't') { fail(); return out; }
            }
            out += *p++;
        }
        if (*p != '"') fail(); else ++p;
        return out;
    }

    std::shared_ptr<V> val() {
        auto v = std::make_shared<V>();
        ws();
        if (*p == '{') {
            ++p; v->t = V::Obj;
            if (eat('}')) return v;
            do { ws(); std::string k = str(); if (!eat(':')) fail(); v->o.push_back({k, val()}); }
            while (ok && eat(','));
            if (!eat('}')) fail();
        } else if (*p == '[') {
            ++p; v->t = V::Arr;
            if (eat(']')) return v;
            do v->a.push_back(val()); while (ok && eat(','));
            if (!eat(']')) fail();
        } else if (*p == '"') {
            v->t = V::Str; v->s = str();
        } else if (!strncmp(p, "true", 4))  { v->t = V::Bool; v->b = true; p += 4; }
        else if (!strncmp(p, "false", 5))   { v->t = V::Bool; p += 5; }
        else if (!strncmp(p, "null", 4))    { p += 4; }
        else if (*p == '-' || (*p >= '0' && *p <= '9')) {
            char* end; v->t = V::Num; v->n = strtod(p, &end);
            if (end == p) fail(); p = end;
        } else fail();
        return v;
    }
};

// ---- build it once ----------------------------------------------------------

struct StrOut : Catalog::Out {
    std::string s;
    void put(const char* d, size_t n) override { s.append(d, n); }
};

static std::string g_json;
static std::shared_ptr<V> g_cat;

static const V& cat() { return *g_cat; }
static const V& list(const char* k) {
    const V* v = cat().get(k);
    TEST_ASSERT_NOT_NULL_MESSAGE(v, k);
    TEST_ASSERT_EQUAL_MESSAGE(V::Arr, v->t, k);
    return *v;
}
static std::string strOf(const V* v) {
    if (!v) return "";
    if (v->t == V::Str) return v->s;
    char b[32]; snprintf(b, sizeof(b), "%g", v->n); return b;
}

// ---- tests ------------------------------------------------------------------

static void test_parses()
{
    TEST_ASSERT_TRUE(g_cat != nullptr);
    TEST_ASSERT_EQUAL(V::Obj, cat().t);
    const V* v = cat().get("v");
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(v->t == V::Num && v->n == 1);
}

static void test_required_keys()
{
    for (const char* k : {"layouts", "styles", "scenes", "panels", "bleModes", "classes",
                          "dests", "actions", "panelKeys", "huKeys", "console", "raise", "logTags"})
        list(k);
    const V* e = cat().get("enums");
    TEST_ASSERT_NOT_NULL(e);
    for (const char* k : {"arrows", "icons", "src", "fmt", "mediaSrc", "canRates", "capModes",
                          "fuelUnits", "tripUnits", "mapSrc", "macroTriggers", "macroEffects",
                          "ledOrder", "ledIdle", "ledReact", "logLevels"}) {
        const V* l = e->get(k);
        TEST_ASSERT_NOT_NULL_MESSAGE(l, k);
        TEST_ASSERT_TRUE_MESSAGE(l->t == V::Arr && !l->a.empty(), k);
        for (const auto& r : l->a) TEST_ASSERT_NOT_NULL_MESSAGE(r->get("v"), k);
    }
}

// The page has the words (web/i18n, both languages): the firmware sends ids,
// and a name only where it is not a word (AF-RDS, 125k, VOL+). Cyrillic in
// UTF-8 leads with 0xD0..0xD3.
static void test_no_ukrainian_in_the_catalog()
{
    size_t at = std::string::npos;
    for (size_t i = 0; i < g_json.size(); ++i) {
        const unsigned char c = (unsigned char)g_json[i];
        if (c >= 0xD0 && c <= 0xD3) { at = i; break; }
    }
    const std::string near = at == std::string::npos ? "" : g_json.substr(at > 60 ? at - 60 : 0, 120);
    TEST_ASSERT_TRUE_MESSAGE(at == std::string::npos, near.c_str());
}

static void unique(const char* listKey, const char* idKey)
{
    std::set<std::string> seen;
    for (const auto& r : list(listKey).a) {
        const std::string id = strOf(r->get(idKey));
        TEST_ASSERT_FALSE_MESSAGE(id.empty(), listKey);
        TEST_ASSERT_TRUE_MESSAGE(seen.insert(id).second, (std::string(listKey) + ": duplicate " + id).c_str());
    }
}

static void test_ids_unique()
{
    unique("layouts", "id");   unique("styles", "id");   unique("scenes", "id");
    unique("panels", "id");    unique("bleModes", "id"); unique("classes", "bit");
    unique("dests", "id");     unique("actions", "id");  unique("panelKeys", "id");
    unique("huKeys", "code");  unique("console", "cmd");   unique("raise", "id");
    for (const auto& kv : cat().get("enums")->o) {
        std::set<std::string> seen;
        for (const auto& r : kv.second->a)
            TEST_ASSERT_TRUE_MESSAGE(seen.insert(strOf(r->get("v"))).second, kv.first.c_str());
    }
}

// The page resolves `enum` in enums first; the one top-level list a field may
// name is `styles`.
static void fieldsResolve(const V& fields, const char* where)
{
    for (const auto& f : fields.a) {
        TEST_ASSERT_NOT_NULL_MESSAGE(f->get("n"), where);
        const V* en = f->get("enum");
        if (!en) continue;
        const bool found = cat().get("enums")->get(en->s.c_str()) || en->s == "styles";
        TEST_ASSERT_TRUE_MESSAGE(found, (std::string(where) + ": no list " + en->s).c_str());
    }
}

static void test_fields_name_existing_lists()
{
    for (const auto& l : list("layouts").a) fieldsResolve(*l->get("params"), "layout params");
    for (const auto& c : list("console").a) fieldsResolve(*c->get("fields"), "console fields");
}

static void test_layout_panels_exist()
{
    std::set<std::string> panels;
    for (const auto& p : list("panels").a) panels.insert(p->get("id")->s);
    for (const auto& l : list("layouts").a) {
        TEST_ASSERT_FALSE(l->get("panels")->a.empty());
        for (const auto& p : l->get("panels")->a) TEST_ASSERT_TRUE(panels.count(p->s));
    }
}

// A held wheel detent has no wire form: only RollUp and RollDown say so.
static void test_wire_hold_only_on_the_detents()
{
    for (const auto& k : list("panelKeys").a) {
        const std::string id = k->get("id")->s;
        const V* wh = k->get("wireHold");
        const bool detent = id == "RollUp" || id == "RollDown";
        TEST_ASSERT_EQUAL_MESSAGE(detent, wh != nullptr, id.c_str());
        if (wh) TEST_ASSERT_TRUE_MESSAGE(wh->t == V::Bool && !wh->b, id.c_str());
    }
}

// CarminatConstants.h ScrollIndicator; 0x0C (MeganeCAN's) is never on the wire.
static void test_arrows_are_the_wire_values()
{
    std::set<int> v;
    for (const auto& a : cat().get("enums")->get("arrows")->a) v.insert((int)a->get("v")->n);
    TEST_ASSERT_TRUE((v == std::set<int>{0x00, 0x07, 0x0B, 0x03}));
}

// The RAV4 card renders these fields of every row (web/js/70-car.js).
static void test_raise_rows_are_complete()
{
    const V& l = list("raise");
    TEST_ASSERT_TRUE(l.a.size() > 90);
    for (const auto& r : l.a) {
        const std::string id = strOf(r->get("id"));
        for (const char* k : {"group", "unit", "cmd", "kind"}) {
            const V* v = r->get(k);
            TEST_ASSERT_TRUE_MESSAGE(v && v->t == V::Str, (id + ": " + k).c_str());
        }
        const std::string kind = r->get("kind")->s;
        TEST_ASSERT_TRUE_MESSAGE(kind == "n" || kind == "b", id.c_str());
        for (const char* k : {"min", "max", "dec"}) {
            const V* v = r->get(k);
            TEST_ASSERT_TRUE_MESSAGE(v && v->t == V::Num, (id + ": " + k).c_str());
        }
        TEST_ASSERT_TRUE_MESSAGE(r->get("min")->n <= r->get("max")->n, id.c_str());
        TEST_ASSERT_TRUE_MESSAGE(r->get("fw") && r->get("fw")->t == V::Bool, id.c_str());
    }
}

static void test_fixed_ids_the_page_uses()
{
    // The plan's fixed literals: scene "custom" last, bleMode "off", action None.
    TEST_ASSERT_EQUAL_STRING("custom", list("scenes").a.back()->get("id")->s.c_str());
    bool off = false;
    for (const auto& m : list("bleModes").a) off |= m->get("id")->s == "off";
    TEST_ASSERT_TRUE(off);
    const V& none = *list("actions").a.front();
    TEST_ASSERT_EQUAL_STRING("None", none.get("id")->s.c_str());
    TEST_ASSERT_EQUAL(V::Null, none.get("class")->t);
    TEST_ASSERT_TRUE(Catalog::bleModeKnown("both"));
    TEST_ASSERT_FALSE(Catalog::bleModeKnown("ancs"));
    TEST_ASSERT_FALSE(Catalog::bleModeKnown(nullptr));
}

// `settings`: the registry as the page draws it. Ids unique, every parent a
// group listed earlier, every type known, Ints with a range, Enums with rows.
static void test_settings_list()
{
    const V& s = list("settings");
    TEST_ASSERT_TRUE(s.a.size() > 10);
    std::set<std::string> ids, groups;
    for (const auto& e : s.a) {
        const std::string id = e->get("id")->s;
        TEST_ASSERT_TRUE_MESSAGE(ids.insert(id).second, id.c_str());
        // No label: the page has the words (web/i18n k.set.<id>, tools/check_i18n.py).
        TEST_ASSERT_NULL_MESSAGE(e->get("label"), id.c_str());
        TEST_ASSERT_FALSE_MESSAGE(e->get("tag")->s.empty(), id.c_str());
        const std::string type = e->get("type")->s;
        if (const V* p = e->get("parent")) TEST_ASSERT_TRUE_MESSAGE(groups.count(p->s), id.c_str());
        if (type == "group") { groups.insert(id); continue; }
        if (type == "int") {
            TEST_ASSERT_NOT_NULL_MESSAGE(e->get("min"), id.c_str());
            TEST_ASSERT_TRUE_MESSAGE(e->get("min")->n <= e->get("max")->n, id.c_str());
        } else if (type == "enum") {
            TEST_ASSERT_TRUE_MESSAGE(e->get("enum") && !e->get("enum")->a.empty(), id.c_str());
        } else if (type == "str") {
            TEST_ASSERT_TRUE_MESSAGE(e->get("max") && e->get("max")->n > 0, id.c_str());
        } else if (type == "table") {
            TEST_ASSERT_TRUE_MESSAGE(e->get("rows") && e->get("rows")->n > 0, id.c_str());
            TEST_ASSERT_TRUE_MESSAGE(e->get("cols") && !e->get("cols")->a.empty(), id.c_str());
            for (const auto& c : e->get("cols")->a) {
                const std::string ct = c->get("type")->s;
                TEST_ASSERT_FALSE_MESSAGE(c->get("id")->s.empty(), id.c_str());
                if (ct == "enum") TEST_ASSERT_TRUE_MESSAGE(c->get("enum") && !c->get("enum")->a.empty(), id.c_str());
                else if (ct == "int") TEST_ASSERT_NOT_NULL_MESSAGE(c->get("min"), id.c_str());
                else TEST_ASSERT_EQUAL_STRING_MESSAGE("bool", ct.c_str(), id.c_str());
            }
        } else {
            TEST_ASSERT_EQUAL_STRING_MESSAGE("bool", type.c_str(), id.c_str());
        }
    }
    // The keys the pages already post to /api/settings are registry ids.
    for (const char* k : {"uartEnabled", "pulseMs", "keyPopupMs", "mediaSource", "ancsPopup",
                          "bleMode", "huTitleCmd", "huTitleFirst"})
        TEST_ASSERT_TRUE_MESSAGE(ids.count(k), k);
}

// `features`: one bool per Catalog::FEATURES row, in its order, and the lists
// that depend on a feature agree with it (the page trusts both).
static void test_features()
{
    const V* f = cat().get("features");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(V::Obj, f->t);
    TEST_ASSERT_EQUAL((int)Catalog::FEATURE_COUNT, (int)f->o.size());
    std::set<std::string> macros;
    for (size_t i = 0; i < Catalog::FEATURE_COUNT; ++i) {
        const Catalog::Feature& row = Catalog::FEATURES[i];
        TEST_ASSERT_EQUAL_STRING(row.id, f->o[i].first.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(V::Bool, f->o[i].second->t, row.id);
        TEST_ASSERT_EQUAL_MESSAGE(row.on, f->o[i].second->b, row.id);
        TEST_ASSERT_EQUAL_MESSAGE(0, strncmp(row.macro, "FEATURE_", 8), row.id);
        TEST_ASSERT_TRUE_MESSAGE(macros.insert(row.macro).second, row.macro);
    }
    auto on = [&](const char* id) { const V* v = f->get(id); return v && v->b; };
    TEST_ASSERT_EQUAL(on("display"), !list("layouts").a.empty());
    TEST_ASSERT_EQUAL(on("display"), !list("console").a.empty());
    TEST_ASSERT_EQUAL(on("display"), !list("panels").a.empty());
    bool usb = false;
    for (const auto& d : list("dests").a) {
        const std::string id = d->get("id")->s;
        const bool built = d->get("built")->b;
        if (id == "uart") TEST_ASSERT_EQUAL(on("uart"), built);
        if (id == "ams" || id == "hid") TEST_ASSERT_EQUAL(on("ble"), built);
        if (id == "res") TEST_ASSERT_EQUAL(on("res"), built);
        if (id == "usb") usb = true;
    }
    TEST_ASSERT_EQUAL(on("usb"), usb);
}

// build/features.json: {id: "FEATURE_X"}. tools/build_sim.py reads each board
// env's flags (build/features/<env>.json, from tools/gen_pages.py) through it,
// so the simulator's envs use these ids without a second list.
static void writeFeatureMacros(const std::filesystem::path& dir)
{
    std::string s = "{";
    for (size_t i = 0; i < Catalog::FEATURE_COUNT; ++i)
        s += std::string(i ? "," : "") + "\"" + Catalog::FEATURES[i].id + "\":\"" + Catalog::FEATURES[i].macro + "\"";
    s += "}\n";
    FILE* f = fopen((dir / "features.json").string().c_str(), "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot write build/features.json");
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

static void writeForTheSimulator()
{
    // pio test runs the program from the project root, so this is <repo>/build.
    std::filesystem::path dir = std::filesystem::current_path() / "build";
    std::filesystem::create_directories(dir);
    FILE* f = fopen((dir / "catalog.json").string().c_str(), "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot write build/catalog.json");
    fwrite(g_json.data(), 1, g_json.size(), f);
    fclose(f);
    printf("catalog: %u bytes -> %s\n", (unsigned)g_json.size(), (dir / "catalog.json").string().c_str());
    writeFeatureMacros(dir);
}

int main(int, char**)
{
    StrOut out;
    Catalog::write(out);
    g_json = out.s;
    Reader r{g_json.c_str()};
    g_cat = r.val();
    r.ws();
    if (!r.ok || *r.p) g_cat = nullptr;

    UNITY_BEGIN();
    RUN_TEST(test_parses);
    if (g_cat) {
        RUN_TEST(test_required_keys);
        RUN_TEST(test_ids_unique);
        RUN_TEST(test_fields_name_existing_lists);
        RUN_TEST(test_layout_panels_exist);
        RUN_TEST(test_arrows_are_the_wire_values);
        RUN_TEST(test_wire_hold_only_on_the_detents);
        RUN_TEST(test_fixed_ids_the_page_uses);
        RUN_TEST(test_raise_rows_are_complete);
        RUN_TEST(test_no_ukrainian_in_the_catalog);
        RUN_TEST(test_settings_list);
        RUN_TEST(test_features);
        RUN_TEST(writeForTheSimulator);
    }
    return UNITY_END();
}
