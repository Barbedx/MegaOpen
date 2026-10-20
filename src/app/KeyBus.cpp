#include "KeyBus.h"
#include <esp_timer.h>
#include "../utils/Locks.h"

namespace App
{
    namespace KeyBus
    {
        namespace
        {
            // ~1.7 kB, static: created before any producer can run, and no
            // allocation can fail at boot or fragment the heap later.
            Keys::KeyQueues       s_q;
            Locks::Spin           s_lock;
            volatile TaskHandle_t s_consumer = nullptr;

            inline void wake()
            {
                const TaskHandle_t t = s_consumer;
                if (t) xTaskNotifyGive(t);
            }
        }

        void setConsumer(TaskHandle_t t) { s_consumer = t; }

        bool post(Keys::Lane lane, const Keys::KeyMsg& m)
        {
            s_lock.lock();
            const bool ok = s_q.push(lane, m);
            s_lock.unlock();
            // Woken even on a drop: a lost release is acted on by the consumer.
            wake();
            return ok;
        }

        bool postEvent(const Keys::Event& e)
        {
            Keys::KeyMsg m;
            m.kind = Keys::KeyMsg::Kind::Event;
            m.ev   = e;
            m.atUs = nowUs();
            return post(e.state == Keys::State::Release ? Keys::Lane::Rel : Keys::Lane::Tap, m);
        }

        bool postTap(const Keys::Event& e, Keys::Lane lane)
        {
            Keys::KeyMsg m;
            m.kind = Keys::KeyMsg::Kind::Tap;
            m.ev   = e;
            m.atUs = nowUs();
            return post(lane, m);
        }

        bool postPanelKey(uint16_t code, bool hold, uint32_t tMs, uint32_t pollGapUs)
        {
            Keys::KeyMsg m;
            m.kind  = Keys::KeyMsg::Kind::PanelKey;
            m.code  = code;
            m.a     = hold ? 1 : 0;
            m.ev.tMs = tMs;
            m.ev.srcId = 3;
            m.atUs  = nowUs();
            m.aux   = pollGapUs;
            return post(Keys::Lane::Tap, m);
        }

        bool postLiteral(uint8_t code, uint8_t state, bool release)
        {
            Keys::KeyMsg m;
            m.kind = Keys::KeyMsg::Kind::Literal;
            m.code = code;
            m.a    = state;
            m.b    = release ? 1 : 0;
            m.ev.srcId = 2;
            m.atUs = nowUs();
            return post(Keys::Lane::Web, m);
        }

        bool postMacroRun(uint8_t idx)
        {
            Keys::KeyMsg m;
            m.kind = Keys::KeyMsg::Kind::MacroRun;
            m.a    = idx;
            m.ev.srcId = 2;
            m.atUs = nowUs();
            return post(Keys::Lane::Web, m);
        }

        void kick() { wake(); }

        bool pop(Keys::KeyMsg& out)
        {
            s_lock.lock();
            const bool ok = s_q.pop(out);
            s_lock.unlock();
            return ok;
        }

        bool empty()
        {
            s_lock.lock();
            const bool e = s_q.empty();
            s_lock.unlock();
            return e;
        }

        uint8_t takeLostReady(Keys::Event* out, uint8_t max, bool& releaseAll)
        {
            s_lock.lock();
            const uint8_t n = s_q.takeLostReady(out, max, releaseAll);
            s_lock.unlock();
            return n;
        }

        uint32_t drops(Keys::Lane l)
        {
            s_lock.lock();
            const uint32_t n = s_q.drops(l);
            s_lock.unlock();
            return n;
        }

        uint32_t tapDropsFrom(uint8_t srcId)
        {
            s_lock.lock();
            const uint32_t n = s_q.tapDropsFrom(srcId);
            s_lock.unlock();
            return n;
        }
    }
}
