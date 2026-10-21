#pragma once
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Which requests the board serves, before any route runs (WebUi::dispatch and
// the OTA endpoints' filter). SECURITY.md has the why.
//
//  * Host: an IP literal, or the board's own name (<mdns> or <mdns>.local).
//    A DNS-rebinding page reaches the board under ITS domain, so its reads
//    are refused. A router's name for the board (megaopen.fritz.box) is not
//    accepted: use the IP or .local.
//  * Writes carry `X-MegaOpen: 1`: every method but GET/HEAD, and GET
//    /ota/start, which starts an update. A browser cannot send a custom header
//    cross-site without a CORS preflight, and the board never allows one, so
//    a page on another site cannot reboot, reset or flash it. Our page adds it
//    (web/js/10-api.js); curl needs `-H 'X-MegaOpen: 1'`.
//
// Pure: host-tested in test_settings.
namespace RequestGate
{
    inline constexpr const char* HEADER = "X-MegaOpen";
    inline constexpr const char* VALUE  = "1";

    enum class Why : uint8_t { Ok, Host, Header };

    inline const char* whyText(Why w)
    {
        switch (w) {
            case Why::Host:   return "this board answers to its IP or its .local name only";
            case Why::Header: return "a change needs the header X-MegaOpen: 1 (the board's own page sends it)";
            default:          return "";
        }
    }

    // "a.b.c.d", each 0..255, nothing else.
    inline bool ipv4(const char* s, size_t n)
    {
        uint8_t dots = 0;
        size_t digits = 0;
        uint16_t part = 0;
        for (size_t i = 0; i < n; ++i) {
            const char c = s[i];
            if (c == '.') {
                if (!digits || ++dots > 3) return false;
                digits = 0; part = 0;
            } else if (c >= '0' && c <= '9') {
                part = (uint16_t)(part * 10 + (c - '0'));
                if (++digits > 3 || part > 255) return false;
            } else {
                return false;
            }
        }
        return dots == 3 && digits;
    }

    inline bool sameName(const char* a, size_t n, const char* b)
    {
        if (strlen(b) != n) return false;
        for (size_t i = 0; i < n; ++i)
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
        return true;
    }

    // `host` is the Host header (port and all); `mdns` the board's name
    // ("megaopen" when empty, as WifiManager does).
    inline bool hostAllowed(const char* host, const char* mdns)
    {
        if (!host || !*host) return false;
        size_t n = strlen(host);
        if (host[0] == '[') return strchr(host, ']') != nullptr;     // an IPv6 literal
        const char* colon = strchr(host, ':');
        if (colon) n = (size_t)(colon - host);
        if (n && host[n - 1] == '.') --n;                             // "megaopen.local."
        if (!n) return false;
        if (ipv4(host, n)) return true;
        const char* name = mdns && *mdns ? mdns : "megaopen";
        if (sameName(host, n, name)) return true;
        const size_t nl = strlen(name);
        return n == nl + 6 && sameName(host, nl, name) && sameName(host + nl, 6, ".local");
    }

    // Needs the header: anything but a read, and GET /ota/start.
    inline bool needsHeader(bool isRead, const char* path)
    {
        if (!isRead) return true;
        return path && strcmp(path, "/ota/start") == 0;
    }

    inline Why check(const char* host, const char* mdns, bool isRead, const char* path,
                     const char* header)
    {
        if (!hostAllowed(host, mdns)) return Why::Host;
        if (needsHeader(isRead, path) && !(header && strcmp(header, VALUE) == 0)) return Why::Header;
        return Why::Ok;
    }
}
