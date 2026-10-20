#include "LogRing.h"
#include "Log.h"

void LogRing::put(uint32_t& at, const uint8_t* p, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) { _b[at] = p[i]; at = adv(at, 1); }
}

void LogRing::get(uint32_t at, uint8_t* p, uint32_t n) const
{
    for (uint32_t i = 0; i < n; ++i) { p[i] = _b[at]; at = adv(at, 1); }
}

uint32_t LogRing::recLen(uint32_t at) const
{
    uint8_t l[2];
    get(at, l, 2);
    return HDR + (l[0] | (l[1] << 8));
}

// Record at `at`: payload into out, ms from its header; the payload length.
uint32_t LogRing::copy(uint32_t at, char* out, uint32_t& ms) const
{
    uint8_t h[HDR];
    get(at, h, HDR);
    const uint32_t n = h[0] | (h[1] << 8);
    ms = (uint32_t)h[2] | ((uint32_t)h[3] << 8) | ((uint32_t)h[4] << 16) | ((uint32_t)h[5] << 24);
    get(adv(at, HDR), (uint8_t*)out, n);
    out[n] = 0;
    return n;
}

bool LogRing::push(const char* s, uint16_t n, uint32_t ms)
{
    const uint32_t need = HDR + n;
    if (_used + need > _size) return false;
    // Room from the kept lines, oldest first. They are enough: _used + need fits.
    while (_kept + _used + need > _size) {
        const uint32_t r = recLen(_old);
        _old = adv(_old, r);
        _kept -= r;
        ++_oldN;
    }
    const uint8_t h[HDR] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)ms, (uint8_t)(ms >> 8),
                            (uint8_t)(ms >> 16), (uint8_t)(ms >> 24)};
    put(_head, h, HDR);
    put(_head, (const uint8_t*)s, n);
    _used += need;
    ++_last;
    return true;
}

int LogRing::take(char* out, uint32_t& ms)
{
    if (!_used) return -1;
    const uint32_t r = HDR + copy(_tail, out, ms);
    _tail = adv(_tail, r);
    _used -= r;
    _kept += r;
    ++_tailN;
    return (int)(r - HDR);
}

int LogRing::read(Cursor& c, char* out, uint32_t& ms, uint32_t& lost, uint32_t maxHops) const
{
    if (c.n > _last) return -1;
    if (c.n < _oldN) {                 // evicted before this reader got to it
        lost += _oldN - c.n;
        c.n = _oldN;
    }
    // No walk yet, or its place was evicted: start at the nearest known record.
    if (c.atN < _oldN || c.atN > c.n) {
        if (c.n >= _tailN) { c.atN = _tailN; c.at = _tail; }
        else               { c.atN = _oldN;  c.at = _old; }
    }
    for (uint32_t hop = 0; c.atN < c.n; ++hop) {
        if (hop == maxHops) return -2;
        c.at = adv(c.at, recLen(c.at));
        ++c.atN;
    }
    const uint32_t n = copy(c.at, out, ms);
    c.at = adv(c.at, HDR + n);
    c.atN = ++c.n;
    return (int)n;
}

LogLine LogLine::parse(const char* s, uint32_t len)
{
    LogLine r{(uint8_t)LogLevel::INF, (uint8_t)(Log::TAG_COUNT - 1), 0};
    static const char LVL[] = "EWIDT";
    if (len >= 3 && s[1] == ' ' && s[2] == '[') {
        for (uint8_t i = 0; i < 5; ++i)
            if (s[0] == LVL[i]) { r.level = i; r.msg = 2; break; }
    }
    const char* p = s + r.msg;
    const char* e = s + len;
    if (p >= e || *p != '[') return r;
    const char* cat = ++p;
    while (p < e && *p != ']' && p - cat < 16) ++p;
    if (p >= e || *p != ']' || p == cat) return r;
    const uint32_t cl = (uint32_t)(p - cat);
    // TAG_ROWS[].cats: space-separated, lower case; the last row is the rest.
    for (uint8_t t = 0; t + 1 < Log::TAG_COUNT; ++t) {
        for (const char* w = Log::TAG_ROWS[t].cats; *w;) {
            const char* we = w;
            while (*we && *we != ' ') ++we;
            if ((uint32_t)(we - w) == cl) {
                uint32_t i = 0;
                while (i < cl && (char)(cat[i] | 0x20) == w[i]) ++i;
                if (i == cl) { r.tag = t; return r; }
            }
            w = *we ? we + 1 : we;
        }
    }
    return r;
}
