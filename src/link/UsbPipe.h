#pragma once
#include <stdint.h>
#include "../Features.h"

// The USB link's transport (docs/design/usb-link.md): a TinyUSB vendor
// interface on the S3's own USB, read and written by the applink task.
namespace UsbPipe
{
    constexpr uint32_t BUS_IDLE_MS = 2000; // no SOF this long: no host drives the bus

    // What the host did to the bus, whatever TinyUSB reports. A dead cable or
    // port leaves sof/addressed off; a host that refused the configuration
    // leaves `addressed` on.
    struct BusStats {
        bool     mounted = false;     // a host configured the device
        bool     sof = false;         // an SOF in the last BUS_IDLE_MS
        bool     addressed = false;   // the host read our descriptors
        bool     suspended = false;
        uint32_t sofAgoMs = 0;        // UINT32_MAX: never
        uint32_t busUps = 0;          // the bus went from idle to SOFs
        uint32_t mounts = 0, unmounts = 0, suspends = 0, resumes = 0;
    };
}

#if FEATURE_USB_LINK
#include "LinkSession.h"
namespace UsbPipe
{
    // The pipe (applink's), after `wake` is known: TinyUSB's rx callback calls it.
    Link::Pipe& begin(void (*wake)());
    // applink, every pass: the bus watch and its log lines.
    void watch(uint32_t now);
    void bus(BusStats& out);
}
#endif
