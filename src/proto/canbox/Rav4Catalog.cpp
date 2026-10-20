#include "Rav4Catalog.h"
#include "Rav4Emitter.h"

namespace Canbox
{
    const char* confName(Conf c)
    {
        switch (c) {
            case Conf::Confirmed: return "confirmed";
            case Conf::High:      return "high";
            case Conf::Medium:    return "medium";
            case Conf::Low:       return "low";
            case Conf::Conflict:  return "CONFLICT";
        }
        return "?";
    }

    // ---- key codes ----------------------------------------------------------
    // The whole documented set; spec table and RZC_ToyotaSeriesProtocol.java agree,
    // hence `High`. No reference emitter sends 0x20, so effect on our unit is
    // untested; see the trials below.
    static const KeyCode kKeys[] = {
        {0x01, "VOL+",              Conf::High, ""},
        {0x02, "VOL-",              Conf::High, ""},
        {0x03, "menu right",        Conf::High, "the unit remaps this to \"down\" in its own UI"},
        {0x04, "menu left",         Conf::High, "...and this to \"up\""},
        {0x07, "SRC / MODE",        Conf::High, "a LONG press of this is MUTE — there is no mute code"},
        {0x08, "voice",             Conf::High, ""},
        {0x09, "answer call",       Conf::High, ""},
        {0x0A, "hang up",           Conf::High, ""},
        {0x0B, "BOX / DISP",        Conf::High, "added in V1.19.006; absent from older firmware"},
        {0x13, "menu up",           Conf::High, ""},
        {0x14, "menu down",         Conf::High, ""},
        {0x15, "back",              Conf::High, ""},
        {0x16, "enter",             Conf::High, ""},
        {0x81, "knob: VOL+",        Conf::High, "rotary: byte 2 is a DETENT COUNT, not a state"},
        {0x82, "knob: VOL-",        Conf::High, "rotary: byte 2 is a detent count"},
        {0x83, "knob: CH/FLD up",   Conf::High, "rotary: byte 2 is a detent count"},
        {0x84, "knob: CH/FLD down", Conf::High, "rotary: byte 2 is a detent count"},
        {0x85, "knob: TUNE/TRACK up",   Conf::Conflict,
               "the constant name says UP, an on-car test found PREVIOUS/restart"},
        {0x86, "knob: TUNE/TRACK down", Conf::Conflict,
               "the constant name says DOWN, an on-car test found NEXT — try this one first"},
        {0x87, "PWR",               Conf::High, "outside the rotary range: an ordinary button"},
        {0x88, "MODE",              Conf::High, "NOT telephone — 0x09/0x0A are the phone keys"},
    };
    static constexpr uint8_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

    uint8_t keyCodeCount() { return kKeyCount; }
    const KeyCode& keyCodeAt(uint8_t i) { return kKeys[i < kKeyCount ? i : 0]; }

    const char* keyCodeName(uint8_t code)
    {
        for (uint8_t i = 0; i < kKeyCount; ++i) if (kKeys[i].code == code) return kKeys[i].name;
        return "?";
    }

    // ---- messages -----------------------------------------------------------
    // Indexed by Rav4Emitter::Msg; the static_assert keeps the rows aligned.
    static const MsgInfo kMsgs[] = {
        /* MSG_DOORS       */ {Conf::Confirmed,
            "Door icons confirmed on our unit at LENGTH 1. The length-2 form adds the "
            "ignition bit, which per the spec gates the trip/fuel/history/TPMS panels — "
            "and risks the icons, because the unit guards on exact length."},
        /* MSG_TEMP        */ {Conf::Confirmed,
            "The 12-byte form is what displays for us. Conflicts with the A/C form below: "
            "same command byte, different length, only one may be live."},
        /* MSG_AC          */ {Conf::Conflict,
            "The vendor spec's 5-byte reading of 0x28, and what the leaked head-unit Java "
            "parses (it drops any 0x28 whose length is not 5). Never confirmed here."},
        /* MSG_STEER       */ {Conf::Confirmed,
            "Working. Sign is still open: the head unit's own code negates before clamping, "
            "the spec does not. If the camera guidelines bend the wrong way, flip the switch."},
        /* MSG_LIGHTS      */ {Conf::Confirmed, "Worked example verifies byte for byte."},
        /* MSG_SPEED       */ {Conf::Confirmed,
            "The trailing pair is AVERAGE speed, not padding. The document's worked example "
            "for this message is the one whose arithmetic does not hold together."},
        /* MSG_ODO         */ {Conf::Confirmed,
            "The F2 08 pair is constant in every capture and its meaning is unknown."},
        /* MSG_RPM         */ {Conf::Confirmed, "x4 on the wire, not /2."},
        /* MSG_TRIP        */ {Conf::High,
            "Elapsed is in MINUTES. Only the range field and the trailing 0x02 are attested "
            "by the source that drove this unit; the first four bytes are the spec's reading."},
        /* MSG_FUEL_INST   */ {Conf::High, "Worked example verifies. Shown only when IG is on."},
        /* MSG_FUEL_HIST   */ {Conf::High,
            "Length 13, not 3. The 3-byte form everyone ships is dropped by the length guard, "
            "which is why the HISTORY screen never populates."},
        /* MSG_RADAR_REAR  */ {Conf::High,
            "Must reach the unit BEFORE any 0x1D: it latches the master-enable bit from here "
            "and discards front-radar frames while that bit is clear."},
        /* MSG_RADAR_FRONT */ {Conf::High,
            "Ordered RIGHT TO LEFT, mirrored relative to the rear message. Distance polarity "
            "is a conflict: if bars fill as you back AWAY, invert the scale."},
        /* MSG_TPMS        */ {Conf::High,
            "Worked example verifies. No sensors on this car, so it is pinning-only."},
        /* MSG_SETTINGS    */ {Conf::High,
            "Never send a steering angle here — that is the VW mapping, and on this profile "
            "it corrupts the settings menu rather than failing."},
        /* MSG_AMP         */ {Conf::High, "Tone controls carry a +7 offset (-5..+5 = 0x02..0x0C)."},
        /* MSG_SYSTEM      */ {Conf::High,
            "Setting b2 (360 camera present) makes the unit SUPPRESS its own radar UI, which "
            "looks exactly like the radar messages not working."},
        /* MSG_ILLUM       */ {Conf::Medium, "Spec only, added in V1.19.004."},
        /* MSG_VSET        */ {Conf::Medium, "Spec only."},
        /* MSG_SEATS       */ {Conf::Medium, "Crown-gated in the head unit's own code; likely inert here."},
        /* MSG_HYBRID      */ {Conf::High,
            "Layout fully recovered and the worked example verifies. Deliberately silent "
            "unless hybrid_present is pinned by hand — a Megane is not a hybrid."},
        /* MSG_VERSION     */ {Conf::High,
            "The best end-to-end liveness proof there is: read it back in Factory Set -> "
            "Display CANBUS Info."},
        /* MSG_COOLANT     */ {Conf::Low,
            "Decoded by k0.a3 (D2 - 40 -> dashboard index 2), never seen in a capture: "
            "scheduled only with the coolant experiment on. Whether anything shows is for "
            "the tablet to say."},
    };
    static_assert(sizeof(kMsgs) / sizeof(kMsgs[0]) == Rav4Emitter::MSG_COUNT,
                  "Rav4Catalog message table is out of step with Rav4Emitter::Msg — it is "
                  "INDEXED by that enum, so a missing row shifts every message after it and "
                  "the UI reports the wrong confidence for the rest of the table.");

    static const MsgInfo kUnknownMsg = {Conf::Low, "?"};
    const MsgInfo& msgInfo(uint8_t i)
    {
        return i < Rav4Emitter::MSG_COUNT ? kMsgs[i] : kUnknownMsg;
    }
}
