#pragma once
#include <stdint.h>

// The LogBus's byte ring, without the lock (LogBus.cpp holds its spinlock
// around every call): pure, so env:native tests the wrap, eviction and loss.
//
// One buffer, two regions, oldest first:
//   [old, tail)   lines the LogBus task has already sent to its sinks, kept
//                 for the web log (GET /api/logs?since=N) until room is needed;
//   [tail, head)  lines still waiting for the sinks.
// A new line evicts kept lines, never waiting ones: when the waiting lines fill
// the ring, push() drops the new line instead (LogBus::dropped()).
//
// Every accepted line gets the next number (1, 2, ...). Records never move once
// written, so a reader's place stays valid until the ring evicts that line;
// read() then skips to the oldest line held and counts the skipped ones lost.
class LogRing
{
public:
    // Record: [len lo][len hi][ms 4 bytes][len bytes], wrapping byte-wise.
    static constexpr uint32_t HDR = 6;

    constexpr LogRing(uint8_t* buf, uint32_t size) : _b(buf), _size(size) {}

    // false: no room among the waiting lines; nothing is written.
    bool push(const char* s, uint16_t n, uint32_t ms);
    // The oldest waiting line into out (room for len + 1), from now on kept for
    // readers; its length, or -1 when nothing waits.
    int  take(char* out, uint32_t& ms);

    // A reader's place: n = the next line it wants (since + 1); at/atN = a
    // record found on the way there and its number (atN 0: none yet).
    struct Cursor { uint32_t n; uint32_t at; uint32_t atN; };

    // Line c.n into out (room for len + 1), and c moves past it. Its length;
    // -1 when the reader is up to date; -2 after maxHops records walked without
    // reaching c.n (c is closer: call again, so the caller can release its lock
    // between steps). Lines evicted before the reader got to them go to lost.
    int  read(Cursor& c, char* out, uint32_t& ms, uint32_t& lost, uint32_t maxHops) const;

    uint32_t last() const { return _last; }   // the newest line's number; 0 before any

private:
    void     put(uint32_t& at, const uint8_t* p, uint32_t n);
    void     get(uint32_t at, uint8_t* p, uint32_t n) const;
    uint32_t recLen(uint32_t at) const;
    uint32_t copy(uint32_t at, char* out, uint32_t& ms) const;
    // n < _size always (a record is at most HDR + MAX_LINE): a compare, not a
    // divide, per byte inside the lock.
    uint32_t adv(uint32_t at, uint32_t n) const { at += n; return at >= _size ? at - _size : at; }

    uint8_t* _b;
    uint32_t _size;
    uint32_t _old = 0, _tail = 0, _head = 0;   // byte offsets
    uint32_t _kept = 0, _used = 0;             // bytes in [old,tail) and [tail,head)
    uint32_t _oldN = 1, _tailN = 1, _last = 0; // the line numbers at old and tail
};

// What the web log shows of a line: level, tag group, where the message starts.
//
// A line is "<L> [cat] msg" from Log::emit (L = E/W/I/D/T), or "[cat] msg"
// from LogBus::line/printf, which carry no level and count as INF. The group
// is cat looked up in Log::TAG_ROWS, case-insensitive; anything else, a line
// with no [cat] included, is the last row ("SYS").
struct LogLine
{
    uint8_t  level;   // LogLevel
    uint8_t  tag;     // index into Log::TAG_ROWS
    uint16_t msg;     // past the level char; [cat] stays in the message
    static LogLine parse(const char* s, uint32_t len);
};
