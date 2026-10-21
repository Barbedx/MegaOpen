// Whole-body gate: without it FEATURE_BLE=0 still links NimBLE in via
// static constructors --gc-sections cannot drop.
#include "Features.h"
#if FEATURE_BLE

#include "bluetooth.h"
#include "utils/Log.h"
#include "ble/BleHub.h"
#include "ble/BlePeerTable.h"
#include "ble/HidRole.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <atomic>

#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>

#include "apple_media_service.h"
#include "apple_notification_service.h"
#include "current_time_service.h"

namespace Bluetooth
{
    namespace
    {
        NimBLEClient *Client = nullptr; // GATT client bound to the AMS peer

        volatile bool Connected = false; // AMS is up
        volatile bool Secured   = false; // link encrypted/bonded
        RTC_DATA_ATTR bool TimeSet = false;

        // Each Apple service is brought up independently and retried until it
        // appears (iOS exposes AMS/ANCS/CTS with slightly different timing).
        bool AmsUp = false, AncsUp = false, CtsUp = false;

        // AMS probe budget, measured from encryption (not connect) so the time
        // the user takes to tap "Pair" does not count.
        const uint32_t SETUP_DEADLINE_MS = 20000;

        // Give up on a peer that never encrypts, so it cannot hold the single
        // GATT client while the real phone waits.
        const uint32_t SECURE_WAIT_MS = 45000;

        // Re-initiate pairing at most this often, and only this many times.
        const uint32_t SECURE_RETRY_MS  = 4000;
        const uint8_t  SECURE_MAX_TRIES = 6;
        uint32_t lastSecureAttempt = 0;

        char        PeerAddr[18] = {0};
        const char *StatusText   = "Pair from iPhone";
        uint32_t    lastSetupAttempt = 0;

        bool startCTS(NimBLEClient *c)
        {
            CurrentTimeService::CurrentTime ct;
            if (!CurrentTimeService::StartTimeService(c, &ct))
                return false;
            if (ct.mYear >= 2020 && ct.mYear <= 2100)
            {
                timeval tv;
                tv.tv_sec  = ct.ToTimeT();
                tv.tv_usec = static_cast<long>(ct.mSecondsFraction * 1000000.0f);
                if (settimeofday(&tv, nullptr) == 0)
                    TimeSet = true;
            }
            return true;
        }

        void resetAmsState()
        {
            Connected = false;
            Secured   = false;
            AmsUp = AncsUp = CtsUp = false;
            Client = nullptr;
            PeerAddr[0] = 0;
        }
    } // namespace

    namespace
    {
        // Set by ble_svc each pass (SetKeysBusy), read by ServiceAms on it.
        bool KeysBusy = false;
        // Requests from other tasks, performed by ServicePending() on ble_svc.
        std::atomic<bool> PeerLostReq{false};
        std::atomic<bool> ClearBondsReq{false};
    }

    static void clearBonds();   // below, beside the public status calls

    void OnAmsPeerLost() { PeerLostReq.store(true); }
    void SetKeysBusy(bool busy) { KeysBusy = busy; }
    void ClearBonds()    { ClearBondsReq.store(true); }

    // ble_svc only: see OnAmsPeerLost() in bluetooth.h.
    static void peerLost()
    {
        resetAmsState();
        // These cache characteristic pointers owned by the lost client; they are
        // freed when the client is rebound, so leaving them set is a use-after-free.
        AppleMediaService::Detach();
        AppleNotificationService::Detach();
        CurrentTimeService::StopTimeService();
        StatusText = HasBond() ? "Waiting for phone" : "Pair from iPhone";
        LOGI("BT", "AMS peer lost — caches detached");
    }

    void ServicePending()
    {
        if (PeerLostReq.exchange(false))
            peerLost();
        if (ClearBondsReq.exchange(false))
            clearBonds();
    }

    void ServiceAms()
    {
#if FEATURE_ANCS
        // Drive deferred ANCS Control Point writes from this (loop) task;
        // a write with response, so not while key work is owed (SetKeysBusy).
        if (!KeysBusy && Connected && Client && Client->isConnected())
            AppleNotificationService::Process();
#endif
        // The whole of a track attribute AMS cut short, one per pass, and
        // only when no key is waiting on this task (SetKeysBusy).
        if (AmsUp && !KeysBusy && Client && Client->isConnected())
            AppleMediaService::Process();

        if (millis() - lastSetupAttempt < 800)
            return;
        lastSetupAttempt = millis();

        // Find a peer worth probing: the known iPhone, or an unclassified newcomer.
        Ble::PeerTable &peers = BleHub::peers();
        Ble::Peer *target = peers.byRole(Ble::PeerRole::Ams);
        if (!target)
            target = peers.byRole(Ble::PeerRole::Unknown);

        if (!target)
        {
            if (Connected || Client)
                peerLost();
            return;
        }

        // GATT calls below block; the host task may reuse this peer's slot
        // meanwhile. Re-fetch by handle after each blocking step.
        const uint16_t h = target->handle;

        // "Fully up" must not wait for a compiled-out service, or discovery
        // re-runs every 800 ms forever.
#if FEATURE_ANCS
        if (AmsUp && AncsUp && CtsUp)
            return; // fully up
#else
        if (AmsUp && CtsUp)
            return; // fully up (ANCS compiled out)
#endif

        if (!Client)
        {
            Client = BleHub::bindAmsClient(*target);
            if (!Client)
                return; // not eligible (head unit, or the single client is taken)
            strncpy(PeerAddr, target->addr, sizeof(PeerAddr) - 1);
            StatusText = "Connecting...";
        }
        if (!Client->isConnected())
            return;

        // Secure first, then discover (NimBLE issue #1033). Async: the sync call
        // blocks the loop task forever until "Pair" is tapped, stalling display,
        // WiFi and HID key release. Encryption shows up in the peer's `encrypted`
        // flag, set by onAuthenticationComplete.
        Secured = target->encrypted;
        if (!Secured)
        {
            if (millis() - target->connectedMs > SECURE_WAIT_MS)
            {
                LOGW("BT", "h=%u never encrypted after %us — not the phone",
                     (unsigned)h, (unsigned)(SECURE_WAIT_MS / 1000));
                target->role = Ble::PeerRole::NotAms;
                target->clientBound = false;
                peerLost();
                return;
            }
            if (target->secureTries < SECURE_MAX_TRIES &&
                millis() - lastSecureAttempt > SECURE_RETRY_MS)
            {
                lastSecureAttempt = millis();
                target->secureTries++;
                LOGI("BT", "Initiating pairing (try %u) — accept the prompt on your iPhone",
                     (unsigned)target->secureTries);
                Client->secureConnection(/*async=*/true);
            }
            return;
        }

        // Past this point the link is encrypted; the probe budget starts here.
        const uint32_t securedAt = target->securedMs ? target->securedMs : target->connectedMs;
        const bool     expired   = millis() - securedAt > SETUP_DEADLINE_MS;

        if (!AmsUp && expired)
        {
            LOGI("BT", "h=%u exposed no AMS %us after encrypting — marking not-ams",
                 (unsigned)h, (unsigned)(SETUP_DEADLINE_MS / 1000));
            target->role        = Ble::PeerRole::NotAms;
            target->clientBound = false;
            peerLost();
            // Encrypted, full probe budget, no AMS: now it may be classified as
            // the HID host (classifyAsHid defers while the probe is live, since
            // an iPhone subscribes to HID too).
            if (target->hidSubscribed)
                BleHub::classifyAsHid(h, target->idAddr);
            return;
        }
        // AMS up but a straggler never appeared (iOS sometimes never exposes ANCS).
        // Stop retrying, or we re-run GATT discovery every 800ms forever.
        if (AmsUp && expired)
            return;

        // Bring up each service independently and keep retrying the missing ones.
        if (!AmsUp && AppleMediaService::StartMediaService(Client))
        {
            // The peer may have gone during discovery — validate before writing to it.
            if (!BleHub::peers().byHandle(h))
            {
                peerLost();
                return;
            }
            AmsUp     = true;
            Connected = true;
            BleHub::classifyAsAms(h); // AMS present == this is the phone; remember it
            StatusText = "Connected";
            LOGI("BT", "AMS started");
        }
        if (!BleHub::peers().byHandle(h))
        {
            peerLost();
            return;
        }
#if FEATURE_ANCS
        if (AmsUp && !AncsUp && AppleNotificationService::StartNotificationService(Client))
        {
            AncsUp = true;
            LOGI("BT", "ANCS started");
        }
#endif
        if (AmsUp && !CtsUp && startCTS(Client))
        {
            CtsUp = true;
            LOGI("BT", "CTS started");
        }
    }

    bool IsConnected() { return Connected && Client && Client->isConnected(); }

    uint16_t ConnIntervalMs()
    {
        if (!Client || !Client->isConnected())
            return 30;
        const uint16_t units = Client->getConnInfo().getConnInterval();   // 1.25 ms each
        return units ? (uint16_t)((units * 5u + 3u) / 4u) : 30;
    }
    bool IsTimeSet()   { return TimeSet; }
    bool HasBond()     { return NimBLEDevice::getNumBonds() > 0; }

    // ble_svc only (ClearBonds() asks for it).
    static void clearBonds()
    {
        // Checked, with a fallback: deleteAllBonds() stops at the first failed
        // ble_gap_unpair() and can leave bonds behind. A leftover bond the phone
        // has forgotten shows as "pairs, then unpairs itself".
        const int before = NimBLEDevice::getNumBonds();
        LOGI("BT", "Clearing all bonds (%d stored; this also drops the head unit's bond)...",
             before);
        const bool ok = NimBLEDevice::deleteAllBonds();
        int after = NimBLEDevice::getNumBonds();
        if (!ok || after > 0)
        {
            // ble_store_clear() wipes the whole security store at once.
            LOGW("BT", "deleteAllBonds returned %s and left %d bond(s) — "
                       "clearing the security store directly",
                 ok ? "true" : "FALSE", after);
            ble_store_clear();
            after = NimBLEDevice::getNumBonds();
        }
        if (after > 0)
            LOGW("BT", "STILL %d bond(s) after clearing — the phone will meet a key "
                       "it does not have; expect pair-then-drop", after);
        else
            LOGI("BT", "bonds %d -> 0", before);
        // Remembered roles outlive bonds; a wrong one (phone recorded as head
        // unit) is never re-probed, so clear them too.
        BleHub::ForgetKnownRoles();
        if (Client && Client->isConnected())
            Client->disconnect();
        // Release client ownership, or bindAmsClient() keeps returning the
        // same client and AMS stays dead until reboot.
        for (uint8_t i = 0; i < Ble::PeerTable::kMax; i++)
            if (Ble::Peer *p = BleHub::peers().at(i))
                p->clientBound = false;
        peerLost();
        StatusText = "Pair from iPhone";
        LOGI("BT", "Bonds cleared. Also 'Forget this device' on the iPhone, then re-pair.");
    }

    const char *GetStatusText() { return StatusText; }

    String GetStatusJson()
    {
        const bool amsOn = BleHub::amsActive();
        const bool hidOn = BleHub::hidActive();
        const bool hidUp = hidOn && Hid::connected();

        Ble::Peer *hidPeer = BleHub::peers().byRole(Ble::PeerRole::Hid);

        char buf[360];
        // Legacy top-level keys mirror the AMS link for existing callers.
        snprintf(buf, sizeof(buf),
                 "{\"mode\":\"%s\""
                 ",\"connected\":%s,\"status\":\"%s\",\"bonded\":%s,\"address\":\"%s\""
                 ",\"ams\":{\"active\":%s,\"connected\":%s,\"secured\":%s,\"address\":\"%s\""
                 ",\"svc\":{\"ams\":%s,\"ancs\":%s,\"cts\":%s}}"
                 ",\"hid\":{\"active\":%s,\"connected\":%s,\"address\":\"%s\"}}",
                 BleHub::mode() == BleHub::Mode::Both ? "both"
                     : (BleHub::mode() == BleHub::Mode::Keyboard ? "keyboard" : "ams"),
                 IsConnected() ? "true" : "false", StatusText,
                 HasBond() ? "true" : "false", IsConnected() ? PeerAddr : "",
                 amsOn ? "true" : "false", IsConnected() ? "true" : "false",
                 Secured ? "true" : "false", IsConnected() ? PeerAddr : "",
                 AmsUp ? "true" : "false", AncsUp ? "true" : "false", CtsUp ? "true" : "false",
                 hidOn ? "true" : "false", hidUp ? "true" : "false",
                 hidPeer ? hidPeer->addr : "");
        return String(buf);
    }
} // namespace Bluetooth

#endif  // FEATURE_BLE
