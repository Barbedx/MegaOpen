#pragma once

// Compile-time feature switches (flash is the scarce resource).
// Override from platformio.ini build_flags, e.g. -D FEATURE_BLE=1.
//
// One CAN controller, but both bus roles are compiled in; `Bus::Role` picks at
// runtime (src/bus/CanRole.h).

#ifndef FEATURE_DISPLAY_CAN
#define FEATURE_DISPLAY_CAN 1     // OEM display over multimedia CAN: keys in, text out
#endif

#ifndef FEATURE_MATRIX
#define FEATURE_MATRIX 1          // 3x3 keypad on the display connector (6 GPIO)
#endif

#ifndef FEATURE_CANBOX_UART
#define FEATURE_CANBOX_UART 1     // Raise canbox to the Android head unit
#endif

#ifndef FEATURE_VEHICLE_CAN
#define FEATURE_VEHICLE_CAN 1     // vehicle bus sniffer + decode -> canbox telemetry
#endif

#ifndef FEATURE_BLE
#define FEATURE_BLE 1             // BLE: AMS now-playing + CTS clock + HID keys
#endif

// iPhone notifications over ANCS. Off: the service is never started and
// `AncsUp` is not waited on.
#ifndef FEATURE_ANCS
#define FEATURE_ANCS 1
#endif

// ANCS is a BLE service: no BLE, no ANCS.
#if FEATURE_ANCS && !FEATURE_BLE
#undef FEATURE_ANCS
#define FEATURE_ANCS 0
#endif

#ifndef FEATURE_RESISTIVE_KEYS
#define FEATURE_RESISTIVE_KEYS 0  // KEY1/KEY2 analog output (needs the I2C pot)
#endif

// Front-panel knobs (encoders, pot) and their addressable LEDs. Off by default:
// on the C3 its pins are the matrix's (src/board/BoardProfile.h).
#ifndef FEATURE_KNOBS
#define FEATURE_KNOBS 0
#endif

// The UART link from board V — the C3 on the car's OBD2 CAN — carrying raw
// frames up to this board (src/link/). ON only where the pins exist.
#ifndef FEATURE_LINK
#define FEATURE_LINK 0
#endif

// A second CAN controller on SPI (MCP2518FD), listen-only on the car's bus: the
// T-2CAN's CAN A. Does on one board what board V + FEATURE_LINK do on two, so
// the two are exclusive.
#ifndef FEATURE_CAN2
#define FEATURE_CAN2 0
#endif
// The log on flash (src/utils/LogFile.*): /log/0.log this boot, 1 the one before.
// Needs a littlefs partition with room — the T-2CAN's 16 MB table, not the
// DevKit's 128 kB.
#ifndef FEATURE_LOGFS
#define FEATURE_LOGFS 0
#endif

// CAN capture to littlefs (src/capture/CanRecorder.*): the car's bus recorded
// in the car, read on the PC with tools/can_dump.py. Shares FEATURE_LOGFS's
// mount and wants PSRAM for its 1 MB ring (32 kB without), so: the T-2CAN.
#ifndef FEATURE_CAPTURE
#define FEATURE_CAPTURE 0
#endif
#if FEATURE_CAPTURE && !FEATURE_LOGFS
#error "FEATURE_CAPTURE needs FEATURE_LOGFS (the littlefs mount it writes to)"
#endif

// Media keys to the head unit as a wired USB keyboard (S3 native USB only; needs
// ARDUINO_USB_MODE=0, i.e. TinyUSB). src/sinks/UsbKeySink.*.
#ifndef FEATURE_USB_HID
#define FEATURE_USB_HID 0
#endif
// MODE=1 is the fixed USB-Serial-JTAG block: it cannot be a keyboard, and
// USB.begin() there takes the PHY from the console. Both lines change together
// (platformio.ini, env:t2can_usbhid).
#if FEATURE_USB_HID && (!defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE != 0)
#error "FEATURE_USB_HID needs ARDUINO_USB_MODE=0 (TinyUSB); see env:t2can_usbhid"
#endif

#if FEATURE_CAN2 && FEATURE_LINK
#error "FEATURE_CAN2 and FEATURE_LINK both feed the vehicle bus; pick one"
#endif

#if !FEATURE_DISPLAY_CAN && !FEATURE_VEHICLE_CAN
// Not an error — a headless canbox with only the GPIO matrix is a valid build.
#endif

