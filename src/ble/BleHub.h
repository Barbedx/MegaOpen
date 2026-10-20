#pragma once
// Sole owner of the BLE stack: NimBLEDevice::init, the security config, the one
// NimBLEServer, the one NimBLEServerCallbacks, the peer table and the advertising
// payload. Roles (AMS client, HID server) plug into it. Nothing else may call
// NimBLEDevice::init()/createServer(): a second call silently keeps the first name
// and makes security/callbacks last-writer-wins.

#include <NimBLEDevice.h>
#include "BlePeerTable.h"

namespace BleHub
{
    enum class Mode : uint8_t
    {
        Ams,      // iPhone only (legacy "ams")
        Keyboard, // head unit only (legacy "keyboard")
        Both,     // iPhone AMS + head-unit HID simultaneously
    };

    // `deviceName` goes in the primary advertising packet: keep it 4 chars. It
    // shares 31 bytes with the 18-byte AMS solicitation and 3 bytes of flags; the
    // proven iPhone-pairing config (MeganeCAN, "MCD1") used 27 bytes.
    // `fullName` goes in the scan response; defaults to `deviceName`.
    void Begin(Mode m, const char *deviceName, const char *fullName = nullptr);

    // Forget learned peer roles (RAM + NVS). Called by Bluetooth::ClearBonds():
    // a remembered role outlives a bond, and a wrong one never self-corrects.
    void ForgetKnownRoles();

    // Pumps both roles. Call from the loop task only — never from a BLE callback.
    void Service();

    Mode            mode();
    NimBLEServer   *server();
    Ble::PeerTable &peers();
    uint8_t         connectedCount();

    inline bool amsActive(Mode m) { return m == Mode::Ams || m == Mode::Both; }
    inline bool hidActive(Mode m) { return m == Mode::Keyboard || m == Mode::Both; }
    bool amsActive();
    bool hidActive();

    // HID role: a peer subscribed to the input report. Marks it as the head unit,
    // saves its identity address, relaxes the link more than the iPhone's.
    void classifyAsHid(uint16_t handle, const char *idAddr);

    // Loop task, once AMS is confirmed on a peer. Not in onAuthenticationComplete:
    // auth completes before AMS discovery can run.
    void classifyAsAms(uint16_t handle);

    // 1 for single-role modes, so they stop advertising once the peer is up.
    uint8_t maxPeers();

    // The only gate on NimBLEServer::getClient(). It keeps ONE client per server
    // and deleteServices() on rebind frees the Apple services' cached
    // characteristics; binding the wrong peer is heap corruption. All call sites
    // go through here.
    NimBLEClient *bindAmsClient(Ble::Peer &p);

    // Identity address of the peer we last classified, "" if never seen.
    const char *knownAmsAddr();
    const char *knownHidAddr();
} // namespace BleHub
