#include "BlePipe.h"
#if FEATURE_BLE_LINK
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>
#include "freertos/stream_buffer.h"
#include "../utils/LogBus.h"
#include "../utils/PsHeap.h"

namespace BlePipe
{
    namespace
    {
        constexpr uint16_t NONE = 0xFFFF;   // BLE_HS_CONN_HANDLE_NONE

        void (*s_wake)() = nullptr;
        NimBLECharacteristic* s_tx = nullptr;
        StreamBufferHandle_t  s_rx = nullptr;
        StaticStreamBuffer_t  s_rxStruct;
        // The subscribed peer. Written on the NimBLE host task, read on applink.
        volatile uint16_t     s_conn = NONE;
        PeerStats             s_st;

        // NimBLE host task. The bytes go to applink through the stream buffer;
        // never block here (the host task runs every other BLE peer too).
        class RxCallbacks : public NimBLECharacteristicCallbacks {
            void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override
            {
                // WRITE_ENC already refuses an unencrypted write at the ATT
                // layer; this is the second lock on the same door.
                if (!ci.isEncrypted()) { ++s_st.unencrypted; return; }
                const NimBLEAttValue v = c->getValue();
                if (!s_rx || !v.size()) return;
                const size_t sent = xStreamBufferSend(s_rx, v.data(), v.size(), 0);
                if (sent < v.size()) s_st.overflow += v.size() - sent;   // the parser resyncs
                if (s_wake) s_wake();
            }
        } s_rxCb;

        class TxCallbacks : public NimBLECharacteristicCallbacks {
            void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& ci, uint16_t sub) override
            {
                const uint16_t h = ci.getConnHandle();
                if (sub & 0x0001) {
                    if (!ci.isEncrypted()) {
                        // Android subscribes before bonding sometimes: the
                        // subscription stays, the link counts once encrypted.
                        LOGI("link", "ble: h=%u subscribed before encryption, waiting for the bond", (unsigned)h);
                    }
                    s_conn = h;
                    ++s_st.subscribes;
                    s_st.mtu = ci.getMTU();
                    snprintf(s_st.addr, sizeof(s_st.addr), "%s", ci.getIdAddress().toString().c_str());
                    LOGI("link", "ble: %s subscribed (h=%u, mtu %u, enc %d)", s_st.addr, (unsigned)h,
                         (unsigned)s_st.mtu, (int)ci.isEncrypted());
                    if (s_wake) s_wake();
                } else if (s_conn == h) {
                    s_conn = NONE;
                    LOGI("link", "ble: h=%u unsubscribed", (unsigned)h);
                }
            }
        } s_txCb;

        // The subscribed peer's link, encrypted, still there.
        bool linkUp(uint16_t h, uint16_t* mtu = nullptr)
        {
            // ble_gap_conn_find, not NimBLEServer::getPeerInfoByHandle: that
            // one logs an error per miss and hands back handle 0 for "none".
            if (h == NONE) return false;
            ble_gap_conn_desc d;
            if (ble_gap_conn_find(h, &d) != 0 || !d.sec_state.encrypted) return false;
            if (mtu) *mtu = ble_att_mtu(h);
            return true;
        }

        class Pipe final : public Link::Pipe {
        public:
            const char* tag() const override { return "ble"; }
            bool up() override
            {
                const bool u = linkUp(s_conn);
                s_st.subscribed = u;
                if (!u && s_rx) xStreamBufferReset(s_rx);   // a half frame of a peer that left
                return u;
            }
            size_t read(uint8_t* buf, size_t n) override
            {
                return s_rx ? xStreamBufferReceive(s_rx, buf, n, 0) : 0;
            }
            // The frame in notifications of MTU-3. A notification the host has
            // no buffer for (its mbufs are busy with earlier ones) is retried
            // until giveUpMs without progress.
            Tx write(const uint8_t* f, size_t n, uint32_t giveUpMs, uint32_t& waitMax) override
            {
                const uint16_t h = s_conn;
                uint16_t mtu = 23;
                if (!s_tx || !linkUp(h, &mtu)) return Tx::Dropped;
                const size_t chunk = mtu > 23 ? mtu - 3 : 20;
                size_t at = 0;
                uint32_t since = millis();
                while (at < n) {
                    const size_t k = n - at < chunk ? n - at : chunk;
                    if (s_tx->notify(f + at, k, h)) {
                        at += k;
                        since = millis();
                        continue;
                    }
                    ++s_st.notifyRetries;
                    const uint32_t w = millis() - since;
                    if (w > waitMax) waitMax = w;
                    if (w > giveUpMs || !linkUp(h)) return at ? Tx::Cut : Tx::Dropped;
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
                return Tx::Ok;
            }
        } s_pipe;
    }

    void addService(NimBLEServer* server)
    {
        if (!server || s_tx) return;
        if (!s_rx) {
            uint8_t* store = (uint8_t*)PsHeap::malloc(RX_BUF + 1);
            if (!store) { LOGW("link", "ble: no memory for the RX buffer, no link over BLE"); return; }
            s_rx = xStreamBufferCreateStatic(RX_BUF, 1, store, &s_rxStruct);
        }
        NimBLEService* svc = server->createService(SERVICE_UUID);
        NimBLECharacteristic* rx = svc->createCharacteristic(
            RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
        rx->setCallbacks(&s_rxCb);
        s_tx = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);
        s_tx->setCallbacks(&s_txCb);
        svc->start();
        s_st.on = true;
        LOGI("link", "ble: service %s", SERVICE_UUID);
    }

    void onDisconnect(uint16_t h)
    {
        if (s_conn == h) {
            s_conn = NONE;
            if (s_wake) s_wake();
        }
    }

    Link::Pipe& pipe(void (*wake)())
    {
        s_wake = wake;
        return s_pipe;
    }

    void stats(PeerStats& out)
    {
        out = s_st;
        out.subscribed = linkUp(s_conn);
    }
}
#endif
