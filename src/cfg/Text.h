#pragma once
#include <stddef.h>
#include <string.h>

// A short text that lives inside the struct that holds it: no heap. A copy
// of Settings is a plain copy, and a reader on another task can never find a
// freed buffer behind a field. The Arduino String it replaces freed its old
// buffer on every assignment, so every read of a Settings String off httpd
// without the settings lock was a use-after-free waiting for its moment
// (the car task's resolveCarSigs, WifiManager on net_svc), and the lock had
// to be held around every route for it.
//
// N counts the NUL: Text<33> holds 32 characters. Longer input is cut.
// Pure (no Arduino): the host tests use it as the board does.
namespace Cfg
{
    template <size_t N>
    class Text {
    public:
        static constexpr size_t CAP = N - 1;   // characters, without the NUL

        Text() { _b[0] = 0; }
        Text(const char* s) { assign(s); }
        Text& operator=(const char* s) { assign(s); return *this; }
        // Arduino's String on the board, std::string in the host tests, a
        // Text of another size: anything with c_str().
        template <class S> Text& operator=(const S& s) { assign(s.c_str()); return *this; }

        void assign(const char* s)
        {
            size_t n = 0;
            if (s) while (n < CAP && s[n]) ++n;
            if (n && s != _b) memmove(_b, s, n);
            _b[n] = 0;
        }

        const char* c_str() const { return _b; }
        size_t      length() const { return strlen(_b); }
        bool        isEmpty() const { return _b[0] == 0; }

        bool operator==(const char* s) const { return s && strcmp(_b, s) == 0; }
        bool operator!=(const char* s) const { return !(*this == s); }
        template <size_t M> bool operator==(const Text<M>& o) const { return strcmp(_b, o.c_str()) == 0; }
        template <size_t M> bool operator!=(const Text<M>& o) const { return !(*this == o); }

    private:
        char _b[N];
    };
}
