#pragma once
#include <stdint.h>

// A latency distribution in microseconds, cheap enough to record on the key
// path: one bit-width count and an increment, no floats, no allocation.
//
// Bucket i holds the values whose bit width is i: bucket 0 is 0, bucket 1 is
// 1, bucket 2 is 2-3, bucket 3 is 4-7 ... so the error is under a factor of
// two, which is the resolution a "p99 < 3 ms" question needs. Percentiles are
// reported as the bucket's upper bound (never optimistic), clamped to the exact
// maximum, which is kept separately.
//
// Single writer. A reader on another task may see a count and a bucket from
// different moments; for a 10 s heartbeat line that is good enough, and the
// writer can take a consistent copy itself when it matters.
class LatencyHist {
public:
    // Bucket 24 holds 2^23 us (8.4 s) and everything above it.
    static constexpr uint8_t BUCKETS = 25;

    void record(uint32_t us)
    {
        uint8_t b = 0;
        for (uint32_t v = us; v; v >>= 1) ++b;
        if (b >= BUCKETS) b = BUCKETS - 1;
        ++_bucket[b];
        ++_n;
        if (us > _max) _max = us;
    }

    void reset()
    {
        for (uint8_t i = 0; i < BUCKETS; ++i) _bucket[i] = 0;
        _n = 0;
        _max = 0;
    }

    uint32_t count() const { return _n; }
    uint32_t max() const   { return _max; }
    uint32_t bucket(uint8_t i) const { return i < BUCKETS ? _bucket[i] : 0; }

    // The smallest bucket bound at or below which `pct` percent of the samples
    // lie. 0 when nothing was recorded.
    uint32_t percentile(uint8_t pct) const
    {
        if (!_n) return 0;
        if (pct > 100) pct = 100;
        // ceil(n * pct / 100), at least one sample.
        uint64_t want = ((uint64_t)_n * pct + 99) / 100;
        if (!want) want = 1;
        uint64_t seen = 0;
        for (uint8_t i = 0; i < BUCKETS; ++i) {
            seen += _bucket[i];
            if (seen >= want) {
                // The last bucket is open-ended: its only honest bound is the max.
                if (i == BUCKETS - 1) return _max;
                const uint32_t upper = i == 0 ? 0u : (uint32_t)((1ull << i) - 1);
                return upper < _max ? upper : _max;
            }
        }
        return _max;
    }

    uint32_t p50() const { return percentile(50); }
    uint32_t p99() const { return percentile(99); }

private:
    uint32_t _bucket[BUCKETS] = {};
    uint32_t _n = 0;
    uint32_t _max = 0;
};
