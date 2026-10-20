#pragma once
#include <atomic>
#include "KeyMap.h"
#include "IKeySink.h"
#include "../input/MatrixScanner.h"
#include "../input/KeyDebouncer.h"
#include "../input/EncoderDecoder.h"

// Portable input path: scan -> debounce -> map -> encoder decode -> sinks.
// main.cpp supplies GPIO callbacks and a clock.
//
// Two halves on two tasks (docs/design/event-tasks.md, Step 2): poll() and the
// map/debounce/encoder state belong to the matrix scan task, and with an emit
// hook set it only hands its events over; dispatch() and the sink chain belong
// to the keys task alone, which takes them off the KeyBus.
namespace Keys
{
    class KeyEngine {
    public:
        // UART, USB HID, the ladder, AMS and BLE HID can all be live at once on
        // one board; 4 left no room, and the fifth was refused without a word.
        static constexpr uint8_t MAX_SINKS = 6;

        // Every dispatched logical event, for the log stream.
        using EventCb = void (*)(const Event& e, void* ctx);
        // Runs ahead of the sinks; returning true consumes the event. Used by
        // macros so the keys spelling a combination don't also act. One filter only.
        using FilterCb = bool (*)(const Event& e, void* ctx);
        // Where poll() sends what the matrix produced, instead of dispatching
        // it on the scan task: the KeyBus, so the keys task is the only one
        // that ever runs the sinks.
        using EmitFn = void (*)(const Event& e, void* ctx);

        void begin(Input::MatrixScanner* scanner, KeyMap* map);
        // False (and logged) when the chain is full: that sink gets no keys.
        bool addSink(ISink* s);
        void onEvent(EventCb cb, void* ctx) { _evCb = cb; _evCtx = ctx; }
        void onFilter(FilterCb cb, void* ctx) { _filter = cb; _filterCtx = ctx; }
        void onEmit(EmitFn cb, void* ctx) { _emit = cb; _emitCtx = ctx; }

        // Re-read everything that depends on the key map (encoder phases, fast
        // debounce mask). Call after the map changes from the web UI.
        void applyMap();

        void setTiming(const Input::KeyDebouncer::Params& p);
        const Input::KeyDebouncer::Params& timing() const { return _deb.params(); }
        void setEncoderInverted(bool inv) { _enc.setInverted(inv); }
        bool encoderInverted() const { return _enc.inverted(); }

        // One iteration: sweep the matrix and dispatch whatever came of it, or
        // hand it to the emit hook when one is set.
        void poll(uint32_t nowMs);

        // Inject an event that did not come from the matrix (web test button).
        void dispatch(const Event& e);
        // Same, skipping the filter: for the filter's own emissions.
        void dispatchRaw(const Event& e);

        uint32_t events() const { return _events.load(std::memory_order_relaxed); }
        uint32_t filtered() const { return _filtered.load(std::memory_order_relaxed); }

    private:
        Input::MatrixScanner* _scanner = nullptr;
        KeyMap*               _map = nullptr;
        Input::KeyDebouncer   _deb;
        Input::EncoderDecoder _enc;

        ISink*   _sinks[MAX_SINKS] = {};
        uint8_t  _sinkCount = 0;
        EventCb  _evCb = nullptr;   void* _evCtx = nullptr;
        FilterCb _filter = nullptr; void* _filterCtx = nullptr;
        EmitFn   _emit = nullptr;   void* _emitCtx = nullptr;

        void deliver(const Event& e) { if (_emit) _emit(e, _emitCtx); else dispatch(e); }
        // Atomic: only the keys task counts, but the loop, httpd and the cloud
        // task read them.
        std::atomic<uint32_t> _events{0};
        std::atomic<uint32_t> _filtered{0};
    };
}
