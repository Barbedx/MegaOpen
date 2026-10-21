#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Row shapes of the lists GET /api/catalog serves (docs/design/web-ui.md, rule 2).
// Each list is declared beside the code that parses and stores its ids, so a
// new style or action is one row there and nothing in the page. Pure: the
// native build writes the simulator's catalog from the same tables.

// `id` is what NVS and the API store; `name` only where the id is not a word
// ("VU", "GRB"): nullptr otherwise, the page has the words (web/i18n k.*).
struct UiName {
    const char* id;
    const char* name;
};

// An enum's name table, row i = value i, walked both ways. Out of range and
// unknown fall back to row 0, the enum's default.
template <size_t N>
const char* uiId(const UiName (&t)[N], uint8_t i) { return t[i < N ? i : 0].id; }

// -1 when s names no row: for a route that must answer 400, not fall back.
template <size_t N>
int uiFind(const UiName (&t)[N], const char* s)
{
    if (s) for (uint8_t i = 0; i < N; ++i) if (!strcmp(s, t[i].id)) return i;
    return -1;
}

template <size_t N>
uint8_t uiIndex(const UiName (&t)[N], const char* s)
{
    const int i = uiFind(t, s);
    return i < 0 ? 0 : (uint8_t)i;
}

// One input of a form the page draws from data: a layout parameter or a
// console command field. Exactly one of the three kinds:
//   text  max = characters
//   num   min..max, step (0 = 1), optional hint
//   enum  `en` names a catalog list (enums.<en>, or a top-level list: styles)
struct UiField {
    const char* n;
    const char* label;
    const char* en;
    int32_t     min;
    int32_t     max;
    uint16_t    step;
    bool        num;
    const char* hint;
};

constexpr UiField uiText(const char* n, const char* label, int32_t max)
{ return UiField{n, label, nullptr, 0, max, 0, false, nullptr}; }

constexpr UiField uiNum(const char* n, const char* label, int32_t min, int32_t max,
                        uint16_t step = 0, const char* hint = nullptr)
{ return UiField{n, label, nullptr, min, max, step, true, hint}; }

constexpr UiField uiEnum(const char* n, const char* label, const char* en)
{ return UiField{n, label, en, 0, 0, 0, false, nullptr}; }
