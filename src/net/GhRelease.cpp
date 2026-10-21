#include "GhRelease.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace GhRelease
{
    namespace
    {
        // Digits into a uint16_t; false on none, or on an overflow.
        bool num(const char*& p, uint16_t& out)
        {
            if (*p < '0' || *p > '9') return false;
            uint32_t v = 0;
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (uint32_t)(*p++ - '0');
                if (v > 0xFFFF) return false;
            }
            out = (uint16_t)v;
            return true;
        }

        int hex(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        bool copy(char* dst, size_t n, const char* src)
        {
            if (!src || strlen(src) >= n) return false;
            strcpy(dst, src);
            return true;
        }
    }

    bool parse(const char* s, Ver& out)
    {
        if (!s) return false;
        const char* p = s;
        if (*p == 'v' || *p == 'V') ++p;
        Ver v;
        if (!num(p, v.maj) || *p++ != '.') return false;
        if (!num(p, v.min) || *p++ != '.') return false;
        if (!num(p, v.pat)) return false;
        if (*p == '-') {
            if (strncmp(p, "-rc", 3) != 0) return false;
            p += 3;
            uint16_t rc = 0;
            if (!num(p, rc) || rc > 0x7FFF) return false;
            v.rc = (int16_t)rc;
        }
        if (*p) return false;
        out = v;
        return true;
    }

    int compare(const Ver& a, const Ver& b)
    {
        if (a.maj != b.maj) return a.maj < b.maj ? -1 : 1;
        if (a.min != b.min) return a.min < b.min ? -1 : 1;
        if (a.pat != b.pat) return a.pat < b.pat ? -1 : 1;
        if (a.rc == b.rc) return 0;
        if (a.rc < 0) return 1;           // the final is above every rc of it
        if (b.rc < 0) return -1;
        return a.rc < b.rc ? -1 : 1;
    }

    void filter(JsonDocument& f)
    {
        f.clear();
        f["tag_name"]   = true;
        f["draft"]      = true;
        f["prerelease"] = true;
        JsonObject a = f["assets"].add<JsonObject>();
        a["name"]                 = true;
        a["size"]                 = true;
        a["browser_download_url"] = true;
    }

    Pick pick(JsonVariantConst rel, const char* env, Release& out)
    {
        out = Release{};
        const char* tag = rel["tag_name"] | (const char*)nullptr;
        if (!copy(out.tag, sizeof(out.tag), tag) || !parse(tag, out.ver)) return Pick::NO_TAG;
        if ((rel["draft"] | false) || (rel["prerelease"] | false)) return Pick::DRAFT;

        // The file name carries the version without the 'v' (release.yml's $VER).
        const char* ver = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
        if (snprintf(out.asset, sizeof(out.asset), "megaopen-%s-%s.bin", env, ver) >=
            (int)sizeof(out.asset))
            return Pick::NO_ASSET;

        bool bin = false, sums = false;
        for (JsonVariantConst a : rel["assets"].as<JsonArrayConst>()) {
            const char* name = a["name"] | "";
            const char* url  = a["browser_download_url"] | (const char*)nullptr;
            if (!bin && strcmp(name, out.asset) == 0 && copy(out.binUrl, sizeof(out.binUrl), url)) {
                out.size = a["size"] | 0u;
                bin = true;
            } else if (!sums && strcmp(name, "SHA256SUMS") == 0 &&
                       copy(out.sumsUrl, sizeof(out.sumsUrl), url)) {
                sums = true;
            }
        }
        if (!bin) return Pick::NO_ASSET;
        if (!sums) return Pick::NO_SUMS;
        return Pick::OK;
    }

    const char* pickText(Pick p)
    {
        switch (p) {
            case Pick::OK:       return "ok";
            case Pick::NO_TAG:   return "the latest release has no version tag";
            case Pick::DRAFT:    return "the latest release is a draft or a prerelease";
            case Pick::NO_ASSET: return "the latest release has no image for this board";
            case Pick::NO_SUMS:  return "the latest release has no SHA256SUMS";
        }
        return "?";
    }

    bool trustedUrl(const char* url)
    {
        static const char* const HOSTS[] = {
            "api.github.com", "github.com", "objects.githubusercontent.com",
            "release-assets.githubusercontent.com",
        };
        static const char SCHEME[] = "https://";
        if (!url || strncmp(url, SCHEME, sizeof(SCHEME) - 1) != 0) return false;
        const char* h = url + sizeof(SCHEME) - 1;
        const size_t n = strcspn(h, "/?#");          // the authority
        if (memchr(h, '@', n) || memchr(h, ':', n)) return false;   // no userinfo, no port
        for (const char* host : HOSTS) {
            const size_t k = strlen(host);
            if (n == k && strncasecmp(h, host, k) == 0) return true;
        }
        return false;
    }

    bool sumFor(const char* sums, size_t len, const char* asset, uint8_t out[32])
    {
        const size_t alen = strlen(asset);
        const char* p = sums;
        const char* end = sums + len;
        while (p < end) {
            const char* eol = (const char*)memchr(p, '\n', (size_t)(end - p));
            const char* le  = eol ? eol : end;
            const char* e   = le;
            if (e > p && e[-1] == '\r') --e;
            // 64 hex, a space, then ' ' or '*', then the name to the end of the line.
            if (e - p == (ptrdiff_t)(66 + alen) && p[64] == ' ' && (p[65] == ' ' || p[65] == '*') &&
                memcmp(p + 66, asset, alen) == 0) {
                uint8_t h[32];
                bool ok = true;
                for (int i = 0; i < 32 && ok; ++i) {
                    const int hi = hex(p[2 * i]), lo = hex(p[2 * i + 1]);
                    ok = hi >= 0 && lo >= 0;
                    h[i] = (uint8_t)((hi << 4) | lo);
                }
                if (ok) { memcpy(out, h, 32); return true; }
            }
            p = le + 1;
        }
        return false;
    }
}
