#pragma once
#include <AffaDisplay.h>
#include "KeyEngine.h"
#include "AffaMap.h"

// The OEM display as a key source: AffaDisplay decodes stalk keys from the
// multimedia CAN and they feed the same KeyEngine pipeline as the matrix.
// Key meaning comes from AffaMap (web-editable, NVS). This class only:
//   * synthesises press/release pairs (AFFA sends clicks + hold flag, no release);
//   * passes wheel detents as single events (the sink makes a rotary frame).
//
// Threading: the keys task. The panel's key callback (inside poll(), on the
// panel's task) only posts the raw code to the KeyBus; onPanelKey() maps it
// here, so the panel never waits for a sink. Must not call into the display.
namespace Keys
{
    class AffaKeySource {
    public:
        // One stalk press and what it mapped to, for display (popup, web log).
        // Emitted for every press, including unknown codes and emptied rows.
        struct Report {
            affa::Key key    = affa::Key::Load;  // the raw wire code, always
            bool      hold   = false;
            bool      known  = false;   // one of the eight codes AffaMap names
            bool      mapped = false;   // ...and its row carries an action
            Action    action = Action::None;
            uint8_t   code   = 0;       // the row's override, 0 = profile default
            bool      asLong = false;
        };
        using ReportCb = void (*)(const Report& r, void* ctx);

        // millis() or a test clock, for handle() only: onPanelKey() carries the
        // source time in. Without either, events are stamped tMs = 0 and the
        // UART sink schedules the release against a time long past.
        using NowFn = uint32_t (*)();

        void begin(KeyEngine* engine, const AffaMap* map, NowFn now = nullptr)
        {
            _engine = engine; _map = map; _now = now;
        }

        void onReport(ReportCb cb, void* ctx) { _report = cb; _reportCtx = ctx; }

        // A raw panel key as it crossed the KeyBus: the wire code, whether the
        // panel flagged it held, and its source time (the panel's decode).
        // Plain integers, so the bus itself never needs <AffaDisplay.h>.
        void onPanelKey(uint16_t code, bool hold, uint32_t tMs);

        // Bind as affa::AffaDisplayBase::KeyCb; the instance travels in ctx.
        // Stamps the key with the clock now. Tests, and anything that maps a
        // key on the task that decoded it.
        static void handle(affa::Key k, affa::KeyEdge e, void* ctx);

        uint32_t events() const { return _events; }
        uint32_t unmapped() const { return _unmapped; }
        // Last code with no row, raw (affa::Key is an open enum), for the UI.
        uint16_t lastUnmapped() const { return _lastUnmapped; }
        bool     lastUnmappedHold() const { return _lastUnmappedHold; }

    private:
        void dispatch(affa::Key k, bool hold, uint32_t tMs);

        KeyEngine*     _engine = nullptr;
        const AffaMap* _map = nullptr;
        NowFn          _now = nullptr;
        ReportCb       _report = nullptr;
        void*          _reportCtx = nullptr;
        uint32_t       _events = 0;
        uint32_t       _unmapped = 0;
        uint16_t       _lastUnmapped = 0;
        bool           _lastUnmappedHold = false;
    };
}
