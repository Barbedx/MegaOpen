#pragma once
#include <stdint.h>

// The OTA trial's decision, apart from the flash (src/app/OtaTrial.h does the
// esp_ota_* calls). Portable: the host tests drive it with numbers.
//
// An OTA image boots PENDING_VERIFY. It is confirmed once it has been healthy
// for CONFIRM_MS without a break: one good sample after a bad stretch starts
// the count again. With no confirmation TRIAL_MS after boot it is reverted. An
// image that is not on trial (USB-flashed, or already confirmed) never gets a
// verdict.
class OtaVerdict {
public:
    static constexpr uint32_t CONFIRM_MS = 60000;
    static constexpr uint32_t TRIAL_MS   = 180000;

    enum class Act : uint8_t { None, Confirm, Revert };

    // pendingVerify: the running image's OTA state is PENDING_VERIFY.
    void begin(bool pendingVerify) { _trial = pendingVerify; _done = false; _healthy = false; }

    // Every pass, nowMs since boot. At most one Confirm or Revert per boot.
    Act tick(uint32_t nowMs, bool healthy);

    bool onTrial() const { return _trial && !_done; }

private:
    bool     _trial = false;
    bool     _done = false;
    bool     _healthy = false;     // healthy at the last sample, since _healthySince
    uint32_t _healthySince = 0;
};
