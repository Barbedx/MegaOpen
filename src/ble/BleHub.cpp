// Whole-body gate: otherwise FEATURE_BLE=0 still links NimBLE in via static
// constructors that --gc-sections cannot drop.
#include "../Features.h"
#if FEATURE_BLE

#include "BleHub.h"
#include "HidRole.h"
#include "../bluetooth.h"
#include "../utils/Log.h"

#include <Preferences.h>
#include <string.h>
#include <esp_coexist.h>   // esp_coex_preference_set — see Begin()
// Only to count SoftAP stations on an unencrypted supervision timeout.
#include <WiFi.h>

// Apple Media Service — solicited in the advertising packet so a fresh iPhone
// surfaces us in Settings > Bluetooth.
#define APPLE_MEDIA_SERVICE_UUID "89D3502B-0F36-433A-8EF4-C502AD55F8DC"

namespace BleHub
{
    namespace
    {
        Mode            gMode   = Mode::Ams;
        NimBLEServer   *gServer = nullptr;
        Ble::PeerTable  gPeers;
        // gName: primary adv packet, shares 31 bytes with flags(3) + AMS
        // solicitation(18); keep it to 4 chars. gFullName: scan response.
        char            gName[16] = {0};
        char            gFullName[24] = {0};

        // Identity addresses remembered across boots (NVS "ble") to classify on reconnect.
        char gAmsAddr[18] = {0};
        char gHidAddr[18] = {0};

        void loadKnownAddrs()
        {
            Preferences p;
            if (!p.begin("ble", /*readOnly=*/true))
                return;
            p.getString("ams_addr", gAmsAddr, sizeof(gAmsAddr));
            p.getString("hid_addr", gHidAddr, sizeof(gHidAddr));
            p.end();
        }

        // NVS commits stall tens-hundreds of ms with the cache off; on the NimBLE
        // host task that risks a supervision timeout mid-handshake. Callbacks stage
        // here; Service() (loop task) commits.
        char gPendingAms[18] = {0};
        char gPendingHid[18] = {0};

        void stageKnownAddr(char *pending, const char *dst, const char *value)
        {
            if (!value || !value[0] || Ble::addrEq(dst, value))
                return;
            strncpy(pending, value, 17);
            pending[17] = 0;
        }

        void commitStagedAddrs()
        {
            const bool wantAms = gPendingAms[0] && !Ble::addrEq(gAmsAddr, gPendingAms);
            const bool wantHid = gPendingHid[0] && !Ble::addrEq(gHidAddr, gPendingHid);
            if (!wantAms && !wantHid)
            {
                gPendingAms[0] = gPendingHid[0] = 0;
                return;
            }
            Preferences p;
            if (!p.begin("ble", /*readOnly=*/false))
                return;
            if (wantAms)
            {
                strncpy(gAmsAddr, gPendingAms, 17);
                gAmsAddr[17] = 0;
                p.putString("ams_addr", gAmsAddr);
                LOGI("BLE", "remembered ams_addr = %s", gAmsAddr);
            }
            if (wantHid)
            {
                strncpy(gHidAddr, gPendingHid, 17);
                gHidAddr[17] = 0;
                p.putString("hid_addr", gHidAddr);
                LOGI("BLE", "remembered hid_addr = %s", gHidAddr);
            }
            p.end();
            gPendingAms[0] = gPendingHid[0] = 0;
        }

        // ---- advertising -------------------------------------------------------
        //
        // 31 bytes per packet. Primary: flags(3) + name(2+n) + AMS solicitation(18).
        // In Both mode HID UUID/appearance go in the scan response (Android reads
        // it). The AMS solicitation must stay in the primary: a fresh iPhone does
        // not surface a scan-response-only solicitation.
        // Generic HID (0x03C0) | Keyboard (0x01).
        const uint16_t kAppearanceKeyboard = 0x03C1;

        enum class AdvProfile : uint8_t { AmsOnly, HidOnly, Dual };

        void applyAdvertising(AdvProfile profile)
        {
            NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
            if (!adv)
                return;
            adv->stop();

            NimBLEAdvertisementData advData;
            advData.setFlags(0x06); // LE General Discoverable, BR/EDR not supported
            advData.setName(gName);

            if (profile == AdvProfile::HidOnly)
            {
                advData.setAppearance(kAppearanceKeyboard);
                advData.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
                adv->setAdvertisementData(advData);
                adv->enableScanResponse(false);
            }
            else
            {
                // AMS solicitation, hand-built: addData() takes a complete AD
                // structure with its own length byte -> [0x11][0x15][16-byte UUID].
                NimBLEUUID ams(APPLE_MEDIA_SERVICE_UUID);
                uint8_t sol[18];
                sol[0] = 0x11; // 1 (type) + 16 (uuid)
                sol[1] = 0x15; // 128-bit service solicitation
                memcpy(&sol[2], ams.getValue(), 16);
                advData.addData(sol, sizeof(sol));

                // Name gets at most 8 chars. Overflow silently loses the
                // solicitation iOS needs, so drop the name instead.
                {
                    const size_t nameLen = strlen(gName);
                    const size_t total = 3 + (nameLen ? 2 + nameLen : 0) + sizeof(sol);
                    if (total > 31) {
                        LOGW("BLE", "adv payload %u > 31 bytes with name '%s' — "
                                    "dropping the name to keep the AMS solicitation",
                             (unsigned)total, gName);
                        NimBLEAdvertisementData bare;
                        bare.setFlags(0x06);
                        bare.addData(sol, sizeof(sol));
                        advData = bare;
                    } else {
                        LOGI("BLE", "adv payload %u/31 bytes (name '%s', %u spare)",
                             (unsigned)total, gName, (unsigned)(31 - total));
                        // Only 27 bytes ("MCD1") is proven to pair iPhones; at 31/31
                        // ("MegaOpen") none ever paired. Long names go in the scan response.
                        if (total > 27)
                            LOGW("BLE", "adv payload %u/31 leaves only %u spare — the "
                                        "only iPhone-pairing config ever proven used 27 "
                                        "(a 4-char name). Shorten it.",
                                 (unsigned)total, (unsigned)(31 - total));
                    }
                }
                adv->setAdvertisementData(advData);

                if (profile == AdvProfile::Dual)
                {
                    NimBLEAdvertisementData sr;
                    sr.setAppearance(kAppearanceKeyboard);
                    sr.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
                    // Readable name here, where there is room.
                    if (gFullName[0]) sr.setName(gFullName);
                    adv->setScanResponseData(sr);
                    // m_scanResp defaults false; without this it is never sent.
                    adv->enableScanResponse(true);
                }
                else
                {
                    adv->enableScanResponse(false);
                }
            }

            adv->start();
            LOGI("BLE", "advertising '%s' profile=%s", gName,
                 profile == AdvProfile::Dual ? "dual" : (profile == AdvProfile::HidOnly ? "hid" : "ams"));
        }

        AdvProfile profileForMode()
        {
            switch (gMode)
            {
            case Mode::Keyboard: return AdvProfile::HidOnly;
            case Mode::Both:     return AdvProfile::Dual;
            default:             return AdvProfile::AmsOnly;
            }
        }

        // ---- callbacks ---------------------------------------------------------

        const char *reasonText(int reason)
        {
            switch (reason - 0x200) // strip BLE_HS_ERR_HCI_BASE
            {
            case 0x08: return "supervision timeout (out of range / RF / coexistence)";
            case 0x13: return "remote user terminated";
            case 0x16: return "terminated by local host (normal post-bond drop)";
            case 0x05: return "authentication failure (bond key mismatch)";
            case 0x06: return "PIN/key missing (one side lost the bond!)";
            case 0x3D: return "MIC failure (encryption key mismatch — stale bond!)";
            case 0x22: return "LMP/LL response timeout";
            case 0x3B: return "unacceptable connection parameters";
            default:   return "(see HCI error code)";
            }
        }

        class HubCallbacks : public NimBLEServerCallbacks
        {
            // Host-task callbacks: no getClient() or discovery here; GATT work
            // belongs on the loop task (Service()).
            void onConnect(NimBLEServer *s, NimBLEConnInfo &connInfo) override
            {
                const uint16_t h = connInfo.getConnHandle();
                // toString() is a temporary; copy out.
                char otaBuf[18] = {0};
                char idBuf[18]  = {0};
                strncpy(otaBuf, connInfo.getAddress().toString().c_str(), 17);
                strncpy(idBuf, connInfo.getIdAddress().toString().c_str(), 17);

                if (gPeers.count() >= maxPeers())
                {
                    LOGW("BLE", "mode allows %u peer(s), disconnecting h=%u",
                         (unsigned)maxPeers(), (unsigned)h);
                    s->disconnect(h);
                    return;
                }
                Ble::Peer *p = gPeers.add(h, otaBuf, idBuf);
                if (!p)
                {
                    LOGW("BLE", "peer table full, disconnecting h=%u", (unsigned)h);
                    s->disconnect(h);
                    return;
                }
                p->connectedMs = millis();
                p->encrypted   = connInfo.isEncrypted();

                // Classify by remembered identity address. Ams must be tested
                // first: in Both mode one phone is both, so the same address can be
                // under both keys, and a Hid role means ServiceAms() never probes
                // it. A wrong Ams guess is corrected by SETUP_DEADLINE_MS (NotAms).
                if (Ble::addrEq(p->idAddr, gAmsAddr))
                    p->role = Ble::PeerRole::Ams;
                else if (Ble::addrEq(p->idAddr, gHidAddr))
                    p->role = Ble::PeerRole::Hid;

                LOGI("BLE", "connected h=%u %s id=%s role=%s enc=%d bonds=%d",
                     (unsigned)h, p->addr, p->idAddr, Ble::roleName(p->role),
                     (int)p->encrypted, NimBLEDevice::getNumBonds());

                // Do not relax conn params here: slave latency before pairing,
                // with WiFi on the shared radio, blows the supervision timeout
                // mid-SMP (0x208). Done in onAuthenticationComplete instead.

                // Keep advertising while a slot is free for the second peer.
                if (gPeers.count() < maxPeers())
                    applyAdvertising(profileForMode());
            }

            void onDisconnect(NimBLEServer *, NimBLEConnInfo &connInfo, int reason) override
            {
                const uint16_t h = connInfo.getConnHandle();
                Ble::Peer *p = gPeers.byHandle(h);
                const Ble::PeerRole role = p ? p->role : Ble::PeerRole::Unknown;
                const bool wasClientBound = p && p->clientBound;

                LOGI("BLE", "disconnected h=%u role=%s reason=%d (0x%X = %s) bonds=%d",
                     (unsigned)h, Ble::roleName(role), reason, reason, reasonText(reason),
                     NimBLEDevice::getNumBonds());

                // Supervision timeout before encryption is almost always a SoftAP
                // station hogging the shared radio; say so in the log.
                if ((reason - 0x200) == 0x08 && !(p && p->encrypted))
                {
                    const int sta = WiFi.softAPgetStationNum();
                    if (sta > 0)
                        LOGW("BLE", "%d client(s) on the AP while this link was "
                                    "setting up — that is the most likely reason it "
                                    "timed out. Disconnect them and retry.", sta);
                }

                gPeers.remove(h);

                // Only the AMS peer owns the cached remote characteristics.
                if (wasClientBound)
                    Bluetooth::OnAmsPeerLost();
            }

            void onAuthenticationComplete(NimBLEConnInfo &connInfo) override
            {
                const uint16_t h = connInfo.getConnHandle();
                if (Ble::Peer *p = gPeers.byHandle(h))
                {
                    p->encrypted = connInfo.isEncrypted();
                    if (p->encrypted && !p->securedMs)
                        p->securedMs = millis();
                    // Identity address is only meaningful once bonded (both peers use RPAs).
                    strncpy(p->idAddr, connInfo.getIdAddress().toString().c_str(), 17);
                    p->idAddr[17] = 0;
                    if (p->role == Ble::PeerRole::Hid)
                        stageKnownAddr(gPendingHid, gHidAddr, p->idAddr);
                    // No Ams branch: auth precedes AMS discovery; classifyAsAms() stages it.
                }
                LOGI("BLE", "auth complete h=%u bonded=%d enc=%d auth=%d bonds=%d",
                     (unsigned)h, connInfo.isBonded(), connInfo.isEncrypted(),
                     connInfo.isAuthenticated(), NimBLEDevice::getNumBonds());

                // Relax only after encryption (iOS's ~15-30ms interval starves
                // WiFi). This callback also fires for a link killed mid-pairing,
                // hence the encrypted check. Apple caps supervision at 6s and needs
                // interval*(latency+1)*3 < timeout: 50ms*5*3 = 750ms.
                if (Ble::Peer *q = gPeers.byHandle(h))
                    if (q->encrypted && !q->paramsRelaxed && gServer)
                    {
                        gServer->updateConnParams(h, 24, 40, 4, 600);
                        q->paramsRelaxed = true;
                        LOGI("BLE", "h=%u encrypted — relaxed to 30-50ms latency 4, 6s supervision",
                             (unsigned)h);
                    }
            }
        };

        HubCallbacks gCallbacks;
    } // namespace

    Mode            mode()            { return gMode; }
    NimBLEServer   *server()          { return gServer; }
    Ble::PeerTable &peers()           { return gPeers; }
    uint8_t         connectedCount()  { return gPeers.count(); }
    bool            amsActive()       { return amsActive(gMode); }
    bool            hidActive()       { return hidActive(gMode); }
    const char     *knownAmsAddr()    { return gAmsAddr; }
    const char     *knownHidAddr()    { return gHidAddr; }

    void ForgetKnownRoles()
    {
        gAmsAddr[0] = gHidAddr[0] = 0;
        gPendingAms[0] = gPendingHid[0] = 0;
        Preferences p;
        if (p.begin("ble", /*readOnly=*/false)) {
            p.remove("ams_addr");
            p.remove("hid_addr");
            p.end();
        }
        // Also reset roles of currently connected peers.
        for (uint8_t i = 0; i < Ble::PeerTable::kMax; i++)
            if (Ble::Peer *q = gPeers.at(i)) {
                q->role = Ble::PeerRole::Unknown;
                q->hidSubscribed = false;
            }
        LOGI("BLE", "forgot remembered ams/hid addresses — peers will be re-probed");
    }

    NimBLEClient *bindAmsClient(Ble::Peer &p)
    {
        if (!amsActive() || !gServer)
            return nullptr;
        if (p.clientBound)
            return gServer->getClient(p.handle);
        // Never bind the head unit: getClient() would deleteServices() out from
        // under whatever the AMS peer discovered.
        if (p.role == Ble::PeerRole::Hid || p.role == Ble::PeerRole::NotAms)
            return nullptr;
        // Known head unit, unless also known as the phone (Both mode: one device
        // can be under both keys).
        if (Ble::addrEq(p.idAddr, gHidAddr) && !Ble::addrEq(p.idAddr, gAmsAddr))
            return nullptr;
        // The server holds exactly one client.
        if (gPeers.anyClientBound())
            return nullptr;

        NimBLEClient *c = gServer->getClient(p.handle);
        if (!c)
            return nullptr;
        p.clientBound = true;
        LOGI("BLE", "AMS client bound to h=%u %s", (unsigned)p.handle, p.addr);
        return c;
    }

    void classifyAsHid(uint16_t handle, const char *idAddr)
    {
        Ble::Peer *p = gPeers.byHandle(handle);
        if (!p)
            return;
        p->hidSubscribed = true;

        // A HID subscription does not identify the head unit: iOS subscribes to
        // any bonded BLE keyboard, and in Both mode the phone is both. Absence of
        // AMS (SETUP_DEADLINE_MS in bluetooth.cpp -> NotAms) decides the role.
        if (p->clientBound)
        {
            LOGI("BLE", "h=%u subscribed to HID while its AMS probe is live — "
                        "keeping the probe; AMS presence decides what this is",
                 (unsigned)handle);
        }
        else if (!amsActive() || p->role == Ble::PeerRole::NotAms)
        {
            // No AMS role in this mode, or the deadline already found no AMS.
            p->role = Ble::PeerRole::Hid;
            stageKnownAddr(gPendingHid, gHidAddr, idAddr);
        }
        else
        {
            // Probe not started yet: on reconnect iOS subscribes within ms,
            // before ServiceAms()'s 800 ms tick. Leave the role undecided.
            LOGI("BLE", "h=%u subscribed to HID before its AMS probe started — "
                        "role undecided; AMS presence decides what this is",
                 (unsigned)handle);
        }

        // 100-125ms, latency 4: half the radio duty of the phone params, leaving
        // airtime for WiFi. Worst-case key latency ~250ms.
        if (gServer && !p->hidParamsApplied)
        {
            gServer->updateConnParams(handle, 80, 100, 4, 600);
            p->hidParamsApplied = true;
            p->paramsRelaxed    = true;
        }
    }

    uint8_t maxPeers() { return gMode == Mode::Both ? Ble::PeerTable::kMax : 1; }

    void classifyAsAms(uint16_t handle)
    {
        Ble::Peer *p = gPeers.byHandle(handle);
        if (!p)
            return;
        p->role = Ble::PeerRole::Ams;
        stageKnownAddr(gPendingAms, gAmsAddr, p->idAddr);
    }

    void Begin(Mode m, const char *deviceName, const char *fullName)
    {
        gMode = m;
        strncpy(gName, deviceName ? deviceName : "MCD1", sizeof(gName) - 1);
        // Defaults to the short name.
        strncpy(gFullName, (fullName && *fullName) ? fullName : gName, sizeof(gFullName) - 1);
        loadKnownAddrs();

        LOGI("BLE", "Begin mode=%s name=%s (known ams=%s hid=%s)",
             m == Mode::Both ? "both" : (m == Mode::Keyboard ? "keyboard" : "ams"),
             gName, gAmsAddr[0] ? gAmsAddr : "-", gHidAddr[0] ? gHidAddr : "-");

        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

        // WiFi and BLE share one front end and the default favours WiFi; one SoftAP
        // client then blows BLE supervision timeouts. Prefer BT. The call is
        // deprecated but the status-bit API has no equivalent; log the result.
        {
            const esp_err_t e = esp_coex_preference_set(ESP_COEX_PREFER_BT);
            if (e == ESP_OK)
                LOGI("BLE", "coexistence preference set to BT");
            else
                LOGW("BLE", "coexistence preference NOT set (err %d) — WiFi keeps "
                            "priority on the shared radio; expect pairing to be "
                            "fragile while a client is on the AP", (int)e);
        }

        NimBLEDevice::init(gName);
        NimBLEDevice::setMTU(247); // so ANCS message bodies fit

        // Bonding for every mode: iOS Just Works and Android HOGP both need it.
        NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

        gServer = NimBLEDevice::createServer();
        // deleteCallbacks=false — gCallbacks is a file static, not heap.
        gServer->setCallbacks(&gCallbacks, /*deleteCallbacks=*/false);
        gServer->advertiseOnDisconnect(false); // we drive advertising ourselves

        if (hidActive())
            Hid::begin(gServer);

        // Must happen once, after every role's services exist and before advertising.
        gServer->start();

        applyAdvertising(profileForMode());
        LOGI("BLE", "Begin finished (bonds stored: %d)", NimBLEDevice::getNumBonds());
    }

    void Service()
    {
        if (!gServer)
            return;

        // HID first: its deferred key release must not sit behind AMS's GATT work.
        if (hidActive())
            Hid::service();

        Bluetooth::ServicePending();
        if (amsActive())
            Bluetooth::ServiceAms();

        commitStagedAddrs(); // NVS writes happen here, never in a BLE callback

        // Re-advertise whenever a slot is free (Both mode needs the second peer).
        static uint32_t lastAdv = 0;
        if (gPeers.count() < maxPeers() && millis() - lastAdv > 3000)
        {
            lastAdv = millis();
            NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
            if (adv && !adv->isAdvertising())
                applyAdvertising(profileForMode());
        }
    }
} // namespace BleHub

#endif  // FEATURE_BLE
