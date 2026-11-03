#pragma once

// One hex digit to its value, -1 for anything else. Five copies used to
// exist (the web routes, the car file, the pane picture, the release sums).
// Pure: the host tests use it too.
namespace Hex
{
    inline int nibble(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
}
