#include "SettingsJson.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>

namespace Cfg
{
    namespace SettingsJson
    {
        void Errors::add(const char* fmt, ...)
        {
            if (n >= MAX) { ++more; return; }
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(line[n], LEN, fmt, ap);
            va_end(ap);
            ++n;
        }

        void Errors::join(char* out, size_t cap) const
        {
            if (!cap) return;
            size_t k = 0;
            out[0] = '\0';
            for (uint8_t i = 0; i < n && k + 1 < cap; ++i) {
                const int w = snprintf(out + k, cap - k, "%s%s", i ? "\n" : "", line[i]);
                if (w < 0) break;
                k += (size_t)w < cap - k ? (size_t)w : cap - k - 1;
            }
            if (more && k + 1 < cap) snprintf(out + k, cap - k, "\n... and %u more", (unsigned)more);
        }

        void write(const Model& m, JsonObject out, bool forFile)
        {
            for (size_t i = 0; i < Registry::count(); ++i) {
                const SEntry& e = Registry::at(i);
                if (e.type == SType::Group || !(e.flags & F_WEB) || !Registry::available(m, e)) continue;
                if (forFile && (e.flags & (F_RO | F_VOLATILE))) continue;
                if (e.type == SType::Table) {
                    JsonArray rows = out[e.id].to<JsonArray>();
                    char b[16];
                    for (uint8_t ri = 0; ri < e.tab->rows(); ++ri) {
                        JsonArray row = rows.add<JsonArray>();
                        for (uint8_t c = 0; c < e.tab->cols; ++c) {
                            const SColumn& col = e.tab->col[c];
                            const int32_t v = Registry::cell(m, e, ri, c);
                            const uint8_t dp = Registry::cellDecimals(m, e, ri, c);
                            if (col.type == SType::Bool)      row.add(v != 0);
                            else if (col.type == SType::Enum) row.add(col.en->id((uint8_t)v));
                            else if (dp) row.add(serialized(std::string(Registry::decText(b, sizeof(b), v, dp))));
                            else         row.add(v);
                        }
                    }
                    continue;
                }
                if (e.type == SType::Str) {
                    char b[64];
                    Registry::getText(m, e, b, sizeof(b));
                    out[e.id] = std::string(b);
                    continue;
                }
                const int32_t v = Registry::get(m, e);
                switch (e.type) {
                    case SType::Bool: out[e.id] = v != 0; break;
                    // A stored value outside the rows (uartBaud) has no id:
                    // null, not "", so a file round-trips without a refusal.
                    case SType::Enum:
                        if (Registry::valid(e, v)) out[e.id] = Registry::enumId(e, v);
                        else                       out[e.id] = nullptr;
                        break;
                    default:          out[e.id] = v; break;
                }
            }
        }

        // One table: row i first, each a list of cells in column order. A row
        // or cell not sent, or null, keeps its value.
        static void readTable(Model& m, const SEntry& e, JsonVariantConst j, Read& r, Errors& err)
        {
            if (!j.is<JsonArrayConst>() || j.size() > e.tab->rows()) {
                err.add("%s: a list of %u rows at most", e.id, (unsigned)e.tab->rows());
                return;
            }
            uint8_t ap = 0;
            uint8_t ri = 0;
            for (JsonVariantConst jr : j.as<JsonArrayConst>()) {
                if (!jr.is<JsonArrayConst>()) {
                    err.add("%s %u: a list of cells", e.id, (unsigned)(ri + 1));
                    ++ri;
                    continue;
                }
                uint8_t c = 0;
                for (JsonVariantConst jc : jr.as<JsonArrayConst>()) {
                    if (c >= e.tab->cols) break;
                    const SColumn& col = e.tab->col[c];
                    const char* cw = nullptr;
                    int32_t cv = 0;
                    if (jc.isNull()) { ++c; continue; }
                    if (col.type == SType::Bool) {
                        if (jc.is<bool>()) cv = jc.as<bool>() ? 1 : 0; else cw = "true or false";
                    } else if (col.type == SType::Enum) {
                        const int x = jc.is<const char*>() ? Registry::colEnumIndex(col, jc.as<const char*>()) : -1;
                        if (x < 0) cw = "unknown value"; else cv = x;
                    } else if (!jc.is<double>() ||
                               !Registry::decParse(jc.as<double>(), Registry::cellDecimals(m, e, ri, c), cv)) {
                        cw = "a number";
                    }
                    if (!cw && !Registry::setCell(m, e, ri, c, cv, &ap)) cw = "out of range";
                    if (cw) err.add("%s %u: %s: %s", e.id, (unsigned)(ri + 1), col.id, cw);
                    ++c;
                }
                ++ri;
            }
            if (ap) {
                r.apT |= ap;
                if (r.nTables < sizeof(r.tables) / sizeof(r.tables[0])) r.tables[r.nTables++] = &e;
            }
        }

        void read(Model& m, JsonObjectConst in, Read& r, Errors& err, bool file)
        {
            for (JsonPairConst kv : in) {
                const char* key = kv.key().c_str();
                if (file) {
                    bool forbidden = false;
                    for (const char* f : FILE_FORBIDDEN) if (!strcmp(key, f)) forbidden = true;
                    if (forbidden) { err.add("%s: not allowed in a settings file", key); continue; }
                }
                const SEntry* e = Registry::find(key);
                if (!e || e->type == SType::Group) { ++r.unknown; continue; }
                if (file && (e->flags & F_VOLATILE)) continue;
                const JsonVariantConst j = kv.value();
                // A file carries null for a value with no id (see write());
                // it means "as it is", like a cell.
                if (file && j.isNull()) continue;
                if (file && !(e->flags & F_RO) && !Registry::available(m, *e)) {
                    if (r.nSkipped < sizeof(r.skipped) / sizeof(r.skipped[0])) r.skipped[r.nSkipped++] = e;
                    continue;
                }
                const char* why = nullptr;
                Change c;
                c.e = e;
                if (e->flags & F_RO)                 why = "read-only";
                else if (!Registry::available(m, *e))
                    why = (e->apply & (AP_CAR | AP_HU)) ? "busy, or not on this board" : "not on this board";
                else if (e->type == SType::Table)  { readTable(m, *e, j, r, err); continue; }
                else if (e->type == SType::Str) {
                    c.text = j.is<const char*>() ? j.as<const char*>() : nullptr;
                    if (!Registry::validText(*e, c.text)) why = "printable ASCII text, too long, or not text";
                }
                else if (e->type == SType::Bool) {
                    if (j.is<bool>()) c.v = j.as<bool>() ? 1 : 0; else why = "true or false";
                } else if (e->type == SType::Int) {
                    // Out of range is brought into it, as the handlers before did.
                    if (!j.is<long>()) why = "a number";
                    else {
                        const long x = j.as<long>();
                        const int32_t lo = Registry::minOf(*e), hi = Registry::maxOf(*e);
                        c.v = (int32_t)(x < lo ? lo : x > hi ? hi : x);
                    }
                } else {
                    const int i = j.is<const char*>() ? Registry::enumIndex(*e, j.as<const char*>()) : -1;
                    if (i < 0) why = "unknown value"; else c.v = i;
                }
                if (why) { err.add("%s: %s", e->id, why); continue; }
                if (r.n >= Registry::MAX_CHANGES) {
                    if (!r.tooMany) err.add("too many settings in one request: %u at most", (unsigned)Registry::MAX_CHANGES);
                    r.tooMany = true;
                    continue;
                }
                r.ch[r.n++] = c;
            }
        }

        void apply(Model& m, const Read& r, uint8_t* ap)
        {
            if (ap) *ap |= r.apT;
            for (uint8_t i = 0; i < r.n; ++i) {
                const Change& c = r.ch[i];
                if (c.e->type == SType::Str) Registry::setText(m, *c.e, c.text, ap);
                else                         Registry::set(m, *c.e, c.v, ap);
            }
        }

        bool differs(const Model& m, const Change& c)
        {
            if (c.e->type == SType::Str) {
                char b[64];
                Registry::getText(m, *c.e, b, sizeof(b));
                return !c.text || strcmp(b, c.text) != 0;
            }
            return Registry::get(m, *c.e) != c.v;
        }

        FileCheck checkFile(JsonObjectConst f)
        {
            const char* fmt = f["format"] | "";
            if (strcmp(fmt, FORMAT) != 0 || !f["settings"].is<JsonObjectConst>()) return FileCheck::NotAConfig;
            const int v = f["schema"] | 0;
            if (v > SCHEMA) return FileCheck::Newer;
            if (v < SCHEMA) return FileCheck::Older;
            return FileCheck::Ok;
        }

        const char* fileCheckText(FileCheck c)
        {
            switch (c) {
                case FileCheck::Ok:         return "";
                case FileCheck::NotAConfig: return "not a MegaOpen settings file";
                case FileCheck::Newer:      return "saved by a newer firmware (schema too new): update the board first";
                default:                    return "saved by an older firmware whose settings this one cannot read";
            }
        }
    }
}
