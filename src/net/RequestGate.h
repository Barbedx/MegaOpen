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
//  * Writes carry `X-MegaOpen: 1`: every method but GET/HEAD, and the two
//    GETs that change something (/ota/start starts an update, /api/cap/mark
//    writes into the capture). Every other GET is side-effect-free. A
//    browser cannot send a custom header cross-site without a CORS
//    preflight, and the board never allows one, so
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

    // "[v6]" or "[v6]:port": the ']' last, or followed by ':' and digits.
    inline bool ipv6(const char* s)
    {
        const char* e = strchr(s, ']');
        if (s[0] != '[' || !e || e == s + 1) return false;
        if (!e[1]) return true;
        if (e[1] != ':' || !e[2]) return false;
        for (const char* p = e + 2; *p; ++p) if (*p < '0' || *p > '9') return false;
        return true;
    }

    inline bool nameMatches(const char* host, size_t n, const char* name)
    {
        if (!name || !*name) return false;
        if (sameName(host, n, name)) return true;
        const size_t nl = strlen(name);
        return n == nl + 6 && sameName(host, nl, name) && sameName(host + nl, 6, ".local");
    }

    // `host` is the Host header (port and all); `mdns` the board's name
    // ("megaopen" when empty, as WifiManager does), `alt` another name it
    // still answers to (the one announced at boot, until a restart re-announces
    // a renamed one), or null.
    inline bool hostAllowed(const char* host, const char* mdns, const char* alt = nullptr)
    {
        if (!host || !*host) return false;
        size_t n = strlen(host);
        if (host[0] == '[') return ipv6(host);
        const char* colon = strchr(host, ':');
        if (colon) n = (size_t)(colon - host);
        if (n && host[n - 1] == '.') --n;                             // "megaopen.local."
        if (!n) return false;
        if (ipv4(host, n)) return true;
        return nameMatches(host, n, mdns && *mdns ? mdns : "megaopen") || nameMatches(host, n, alt);
    }

    // The GETs that change something. Keep this list short: a new route that
    // changes state is a POST.
    inline constexpr const char* WRITE_GETS[] = {"/ota/start", "/api/cap/mark"};

    // Needs the header: anything but a read, and the GETs above.
    inline bool needsHeader(bool isRead, const char* path)
    {
        if (!isRead) return true;
        if (!path) return false;
        for (const char* w : WRITE_GETS) if (strcmp(path, w) == 0) return true;
        return false;
    }

    inline Why check(const char* host, const char* mdns, bool isRead, const char* path,
                     const char* header, const char* alt = nullptr)
    {
        if (!hostAllowed(host, mdns, alt)) return Why::Host;
        if (needsHeader(isRead, path) && !(header && strcmp(header, VALUE) == 0)) return Why::Header;
        return Why::Ok;
    }
}
