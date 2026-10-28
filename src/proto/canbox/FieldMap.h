#pragma once
#include <stdint.h>
#include <stddef.h>

// The owner's head-unit field map (docs/design/hu-mapping.md, step 2): which
// value each protocol field sends. A field with no rule sends the car signal of
// its own id (the identity, as before); a rule makes it:
//   SIGNAL  another signal, scaled: value * mul / div (to the nearest) + add
//   CONST   a fixed value
//   OFF     nothing (the field reads as absent: its message leaves it out)
// Fields and signals are Vehicle::Sig ids (the RAV4 fields are Sigs today).
//
// Pure: env:native tests it. NVS keeps it as one blob (pack/unpack), the
// emitter's config carries a copy (App::Hu::EmitterCfg).
namespace Canbox
{
    enum FieldKind : uint8_t { FK_SIGNAL = 1, FK_CONST = 2, FK_OFF = 3 };

    struct FieldRule {
        uint8_t field = 0xFF;
        uint8_t kind  = FK_SIGNAL;
        uint8_t src   = 0xFF;      // FK_SIGNAL: the signal
        int16_t mul   = 1, div = 1;
        int32_t add   = 0;         // FK_SIGNAL: the offset; FK_CONST: the value
    };

    class FieldMap
    {
    public:
        static constexpr uint8_t MAX = 32;
        static constexpr uint8_t BLOB_VER = 1;
        static constexpr size_t  RULE_BYTES = 11;
        static constexpr size_t  BLOB_MAX = 2 + MAX * RULE_BYTES;

        const FieldRule* find(uint8_t field) const;
        // Adds or replaces the field's rule; false when full or not valid
        // (div 0, a SIGNAL rule with no source, a field id 0xFF).
        bool set(const FieldRule& r);
        // Back to the identity.
        void clear(uint8_t field);
        void clearAll() { _n = 0; }
        uint8_t count() const { return _n; }
        const FieldRule& at(uint8_t i) const { return _r[i]; }

        // [ver][n] then n rules; unpack refuses a blob that does not parse
        // (and leaves the map as it was).
        size_t pack(uint8_t* out, size_t cap) const;
        bool   unpack(const uint8_t* in, size_t n);

        // The rule applied to a source value (SIGNAL), with no overflow.
        static int32_t scale(const FieldRule& r, int32_t v);

    private:
        FieldRule _r[MAX];
        uint8_t   _n = 0;
    };
}
