#pragma once
#include <stdint.h>

// The only file in the project that names a pin (ARCHITECTURE.md §10).
// Board is selected by the compiler's target macro, not a project flag, so it
// cannot disagree with the chip:
//   LilyGO T-2CAN(-FD), S3     — two isolated CAN ports; the board in the car.
//   ESP32 DevKit V1 (WROOM-32) — one CAN port (CRX=GPIO5, CTX=GPIO4).
//   ESP32-C3 SuperMini         — board V only (env:boardv).
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ARDUINO_ESP32C3_DEV)
#  define MEGAOPEN_BOARD_C3 1
#else
#  define MEGAOPEN_BOARD_C3 0
#endif
// The only S3 this project builds for is the T-2CAN, so the chip names the board.
// A second S3 board would need a flag here, not a guess.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#  define MEGAOPEN_BOARD_T2CAN 1
#else
#  define MEGAOPEN_BOARD_T2CAN 0
#endif

namespace Board
{
#if MEGAOPEN_BOARD_C3
    // ESP32-C3 SuperMini. Exposed GPIO: 0-10, 20, 21.
    // USB-CDC-on-boot moves the console to USB, which frees the UART0 pins
    // (20/21) for the head-unit link.

    // --- key matrix: 3 rows driven low one at a time, 3 columns read ---------
    // Rows are open-drain/Hi-Z, never driven HIGH: two simultaneous presses
    // would otherwise short two driven rows together.
    inline constexpr uint8_t MATRIX_ROWS = 3;
    inline constexpr uint8_t MATRIX_COLS = 3;
    inline constexpr int8_t  PIN_ROW[MATRIX_ROWS] = {5, 6, 7};
    inline constexpr int8_t  PIN_COL[MATRIX_COLS] = {0, 1, 10};

    // --- CAN (display multimedia bus, or the OBD2 port) ---------------------
    // Read as pin -> module label, never as "rx/tx":
    //     GPIO3  <--  CRX      the SN65HVD230's R output. We read it.
    //     GPIO4  -->  CTX      the SN65HVD230's D input.  We drive it.
    // Module has 120 ohm termination + 10k on Rs. Both C3 boards (hand-wired and
    // carrier) are wired this way; on the carrier it routes as straight tracks.
    // esp32_can's setCANPins(rxPin, txPin) takes RX first; swapped pins fail
    // silently (zero error counters, TX queue never drains) and would drive
    // CRX from the controller's TX pad.
    // A default, not a fact: Cfg::Settings::canRxPin / canTxPin override and persist.
    inline constexpr int8_t PIN_CAN_RX = 3;   // <- CRX
    inline constexpr int8_t PIN_CAN_TX = 4;   // -> CTX

    // The carrier board's pairing, offered as a choice by /api/can/pins.
    inline constexpr int8_t PIN_CAN_RX_CARRIER = 3;
    inline constexpr int8_t PIN_CAN_TX_CARRIER = 4;

    // --- head-unit UART (Raise canbox, 38400 8N1) ---------------------------
    inline constexpr int8_t PIN_HU_TX = 21;
    inline constexpr int8_t PIN_HU_RX = 20;

    // --- I2C: analog front end + the KEY1/KEY2 digital pot (M4, unpopulated) --
    // ADS1115 reads 12V BAT, 12V ACC, the illumination dimmer and the outdoor NTC;
    // MCP4551 drives KEY1/KEY2 later. A bus because no pins are left and ADC2 is
    // unusable while WiFi is on. 8 and 9 are strapping pins that must read high
    // at boot, which the bus pull-ups guarantee. See ARCHITECTURE.md §7.2.
    inline constexpr int8_t PIN_I2C_SDA = 8;
    inline constexpr int8_t PIN_I2C_SCL = 9;

    // Single discrete 12V input (position lights) via optocoupler. Also strapping:
    // the pull-up keeps it high through reset.
    inline constexpr int8_t PIN_DIN_LIGHTS = 2;

    // --- KEY1 / KEY2 resistor ladder (FEATURE_RESISTIVE_KEYS) ----------------
    // Only three pins left: one channel, three legs (three rungs + four combos).
    // 2, 8, 9 are strapping pins that must read HIGH at reset; an idle leg is
    // Hi-Z and the head unit's KEY pull-up holds it high. A leg must never be
    // left driven low across a reset, hence allOff() in begin().
    inline constexpr uint8_t RES_LEGS_CH0 = 3;
    inline constexpr uint8_t RES_LEGS_CH1 = 0;
    inline constexpr int8_t  PIN_RES_CH0[5] = {8, 9, 2, -1, -1};
    inline constexpr int8_t  PIN_RES_CH1[5] = {-1, -1, -1, -1, -1};

    // --- front-panel knobs and their backlight (FEATURE_KNOBS) ---------------
    // These reuse the matrix pins: with the OEM panel on CAN the matrix is idle.
    // Matrix + knobs does not fit a C3 (only strapping pins 2/8/9 remain);
    // KnobService.cpp static_asserts the overlap.
    //
    //     GPIO5  <--  S1       GPIO6  <--  S2       GPIO7  <--  KEY
    //     GPIO0  <--  the pot's wiper (ADC1_CH0 — ADC2 is dead while WiFi is up)
    //     GPIO10 -->  DIN of the first LED
    //
    // The encoder module's "5V" pin goes to 3V3: its 10k pull-ups would put 5 V
    // on non-tolerant inputs. Pot ends to 3V3/GND too. Only the LEDs get 5 V.
    inline constexpr int8_t PIN_KNOB_S1  = 5;
    inline constexpr int8_t PIN_KNOB_S2  = 6;
    inline constexpr int8_t PIN_KNOB_KEY = 7;
    // No pins left for a second encoder. -1 = not fitted.
    inline constexpr int8_t PIN_KNOB2_S1  = -1;
    inline constexpr int8_t PIN_KNOB2_S2  = -1;
    inline constexpr int8_t PIN_KNOB2_KEY = -1;
    inline constexpr int8_t PIN_KNOB_POT = 0;
    inline constexpr int8_t PIN_KNOB_LED = 10;
    // No link as the brain. As board V it uses 20/21 (PIN_HU_TX/RX; src/LinkMain.cpp).
    inline constexpr int8_t PIN_LINK_RX = -1;
    inline constexpr int8_t PIN_LINK_TX = -1;
    // Internal pull-ups on so an unplugged knob reads as at rest, not noise.
    inline constexpr bool   KNOB_PULLUPS = true;
    // Knobs on GPIOs, not an I2C expander (see the T-2CAN branch).
    inline constexpr bool    KNOBS_I2C = false;
    inline constexpr int8_t  PCF_BIT_S1  = -1, PCF_BIT_S2  = -1, PCF_BIT_KEY  = -1;
    inline constexpr int8_t  PCF_BIT2_S1 = -1, PCF_BIT2_S2 = -1, PCF_BIT2_KEY = -1;

#elif MEGAOPEN_BOARD_T2CAN   // ---- LilyGO T-2CAN(-FD), ESP32-S3-WROOM-1U N16R8 ----
    // From the vendor schematic (github.com/Xinyuan-LilyGO/T-2Can, project/*.pdf).
    // Fixed by the board, not ours to choose:
    // A and B are the SILKSCREEN's names (terminal "A" sits by the antenna):
    // the schematic routes IO6/IO7 through TXB_CAN/RXB_CAN to terminal B and the
    // SPI controller's TXCAN/RXCAN through TXA_CAN/RXA_CAN to terminal A.
    //     GPIO6/7   CAN B: the S3's own TWAI, isolated TD501MCAN transceiver
    //     GPIO10-13 SPI to CAN A (MCP2515 or MCP2518FD); 8 INT, 9/3 INT0/INT1 or RST
    //     GPIO19/20 native USB (the console, later HID)
    //     GPIO43/44 UART0, on one of the SH1.0 sockets
    // Never usable here:
    //     26-32 SPI flash; 33-37 OCTAL PSRAM on N16R8 — 35-37 are on the header
    //     anyway, and touching them crashes the board.
    //     0, 3, 45, 46 strapping.
    // What is left on the 2x13 header and SH sockets:
    //     1 2 4 5 14 15 16 17 18 21 38 39 40 41 42 47 48 — every one has a pull-up.

    // No matrix: the panel is on CAN B and delivers its keys there. The pins it
    // would take are the ladder's below.
#  if defined(FEATURE_MATRIX) && FEATURE_MATRIX
#    error "T-2CAN has no matrix pins (the ladder has them); build with FEATURE_MATRIX=0"
#  endif
    inline constexpr uint8_t MATRIX_ROWS = 3;
    inline constexpr uint8_t MATRIX_COLS = 3;
    inline constexpr int8_t  PIN_ROW[MATRIX_ROWS] = {-1, -1, -1};
    inline constexpr int8_t  PIN_COL[MATRIX_COLS] = {-1, -1, -1};

    // --- CAN B, the display's multimedia bus --------------------------------
    // TX GPIO7 -> the transceiver's TXD (via a level shifter), RX GPIO6 <- RXD.
    // pin_config.h: CAN_TX 7, CAN_RX 6. The isolator adds loop delay: if the
    // panel will not register, look at the sample point (TwaiBus::setTiming)
    // before the wiring.
    inline constexpr int8_t PIN_CAN_RX = 6;
    inline constexpr int8_t PIN_CAN_TX = 7;
    inline constexpr int8_t PIN_CAN_RX_CARRIER = 6;
    inline constexpr int8_t PIN_CAN_TX_CARRIER = 7;

    // ---- THE QWIIC LAYOUT (owner, 2026-10-07): nothing soldered to U8 but the
    // LED ring's three pads. hardware/schematic/t2can-qwiic.artifact.html is the drawing.

    // --- head-unit UART (Raise canbox, 38400 8N1): QWIIC 1 ------------------
    // The socket silk-labelled GND 3V3 TX RX is UART0's pins. The console is on
    // USB, so they are free — except that the ROM and the bootloader print their
    // boot log on GPIO43 before setup() runs (BOOT_ROM_LOG_ALWAYS_ON): the head
    // unit sees ~200 bytes of junk at 115200 after EVERY reset — power-up, but
    // also a panic, a watchdog, a brownout, an OTA or rollback reboot — in the
    // middle of a drive if the firmware crashes. Its checksummed framing should
    // discard it. A car unit can silence the ROM for good by burning the
    // UART_PRINT_CONTROL efuse (espefuse); the pins stay as they are.
    inline constexpr int8_t PIN_HU_TX = 43;   // -> head unit RX
    inline constexpr int8_t PIN_HU_RX = 44;   // <- head unit TX

    // --- I2C: QWIIC 2, silk-labelled GND 3V3 IO01 IO02 -----------------------
    // Standard QWIIC order, so a stock cable lands SDA/SCL where they belong.
    inline constexpr int8_t PIN_I2C_SDA = 1;
    inline constexpr int8_t PIN_I2C_SCL = 2;

    // No pin left once the knobs are fitted.
    inline constexpr int8_t PIN_DIN_LIGHTS = -1;

    // --- KEY1 / KEY2 resistor ladder (FEATURE_RESISTIVE_KEYS) ----------------
    // Same split as the DevKit: five legs on KEY1, one on KEY2. None of these
    // is a strapping pin, so a leg the head unit pulls high cannot upset a reset.
    inline constexpr uint8_t RES_LEGS_CH0 = 5;
    inline constexpr uint8_t RES_LEGS_CH1 = 1;
    inline constexpr int8_t  PIN_RES_CH0[5] = {4, 5, 15, 16, 21};
    inline constexpr int8_t  PIN_RES_CH1[5] = {14, -1, -1, -1, -1};

    // --- front-panel knobs and their backlight (FEATURE_KNOBS) ---------------
    // Both encoders through a PCF8574 on QWIIC 2: the owner's LCD I2C backpack,
    // read as one byte per ms by the knob task (Knobs::Service). Its P3 drives
    // the backpack's backlight transistor and always reads low, so it is skipped.
    //     P0 <- right S1   P1 <- right S2   P2 <- right KEY      (LCD pins 4 5 6)
    //     P4 <- left B     P5 <- left button  P6 <- left A       (LCD pins 11 12 13)
    //     P7  spare (LCD pin 14)
    // The left is as the owner's backpack is soldered (2026-10-19): its three
    // wires went on one pin early, 10 11 12, and pin 10 (D3) is not on the
    // PCF8574, so the wire from 10 moved to 13 and the bits follow the wires.
    // Address probed at boot: 0x20-0x27 (PCF8574) and 0x38-0x3F (PCF8574A).
    // The LEDs stay on a GPIO — the one thing an I2C expander cannot time:
    //     GPIO47 (U8 pad 19) -> DIN of the first LED; 5 V from U8 pad 3.
    inline constexpr bool    KNOBS_I2C = true;
    inline constexpr int8_t  PCF_BIT_S1  = 0, PCF_BIT_S2  = 1, PCF_BIT_KEY  = 2;
    inline constexpr int8_t  PCF_BIT2_S1 = 6, PCF_BIT2_S2 = 4, PCF_BIT2_KEY = 5;
    inline constexpr int8_t PIN_KNOB_S1  = -1;
    inline constexpr int8_t PIN_KNOB_S2  = -1;
    inline constexpr int8_t PIN_KNOB_KEY = -1;
    inline constexpr int8_t PIN_KNOB2_S1  = -1;
    inline constexpr int8_t PIN_KNOB2_S2  = -1;
    inline constexpr int8_t PIN_KNOB2_KEY = -1;
    inline constexpr int8_t PIN_KNOB_POT = -1;
    inline constexpr int8_t PIN_KNOB_LED = 47;

    // --- CAN A: MCP2518FD on SPI (FEATURE_CAN2), the car's bus ---------------
    // pin_config.h. 40 MHz crystal on the controller.
    inline constexpr int8_t PIN_CAN2_SCK  = 12;
    inline constexpr int8_t PIN_CAN2_MOSI = 11;
    inline constexpr int8_t PIN_CAN2_MISO = 13;
    inline constexpr int8_t PIN_CAN2_CS   = 10;
    inline constexpr int8_t PIN_CAN2_INT  = 8;

    // CAN A replaces board V and its UART link on this board.
    inline constexpr int8_t PIN_LINK_RX = -1;
    inline constexpr int8_t PIN_LINK_TX = -1;
    // Unlike 34-39 on the DevKit, every S3 pin has an internal pull-up.
    inline constexpr bool   KNOB_PULLUPS = true;

#else   // ---- ESP32 DevKit V1 (classic WROOM-32) — the proven board ----------

    // GPIO 6-11 are the SPI flash; using one crash-loops the board.
    // 34-39 are input only with no pull-ups, so they cannot be matrix columns.
    inline constexpr uint8_t MATRIX_ROWS = 3;
    inline constexpr uint8_t MATRIX_COLS = 3;
    inline constexpr int8_t  PIN_ROW[MATRIX_ROWS] = {25, 26, 27};
    inline constexpr int8_t  PIN_COL[MATRIX_COLS] = {32, 33, 14};

    // --- CAN ---------------------------------------------------------------
    // CRX = GPIO5, CTX = GPIO4. Verified: AffaDisplay's 15_iphone holds
    // `phase Ready` for hours on it with zero error counters.
    inline constexpr int8_t PIN_CAN_RX = 5;
    inline constexpr int8_t PIN_CAN_TX = 4;

    // No second wiring here; named so the settings API has one shape.
    inline constexpr int8_t PIN_CAN_RX_CARRIER = 5;
    inline constexpr int8_t PIN_CAN_TX_CARRIER = 4;

    // --- head-unit UART (Raise canbox, 38400 8N1) ---------------------------
    // UART2 on 16/17. UART0 (1/3) stays the console: no USB-CDC on this part.
    inline constexpr int8_t PIN_HU_TX = 17;
    inline constexpr int8_t PIN_HU_RX = 16;

    // --- I2C ---------------------------------------------------------------
    // The classic ESP32 defaults, so a stock library that assumes them works.
    inline constexpr int8_t PIN_I2C_SDA = 21;
    inline constexpr int8_t PIN_I2C_SCL = 22;

    inline constexpr int8_t PIN_DIN_LIGHTS = 23;

    // --- KEY1 / KEY2 resistor ladder (FEATURE_RESISTIVE_KEYS) ----------------
    // KEY1 -> DUDU7 G7 ("кнопки руля, вход 1"), KEY2 -> G6 ("вход 2"),
    // ground -> G1. Two independent head-unit ADC inputs.
    //
    // Free after matrix/CAN/UART: 13, 18, 19, and (display-less builds) 21-23.
    // The head unit pulls a Hi-Z leg high through its resistor, so avoid:
    //   6-11  SPI flash.
    //   34-39 input only; cannot pull a leg down.
    //   12    MTDI — must read LOW at reset or the flash voltage is wrong and
    //         the board does not boot.
    //   0,1,3 UART0 console.
    //   2     must be low/floating for download mode.
    //   15    MTDO — would work; kept in reserve (also silences the boot log).
    //
    // Five legs on KEY1, one on KEY2: KEY2 behaviour varies by unit and reports
    // conflict; KEY1 is the only input known to work. The KEY2 leg is an experiment.
    inline constexpr uint8_t RES_LEGS_CH0 = 5;
    inline constexpr uint8_t RES_LEGS_CH1 = 1;
    inline constexpr int8_t  PIN_RES_CH0[5] = {13, 18, 19, 23, 21};
    inline constexpr int8_t  PIN_RES_CH1[5] = {22, -1, -1, -1, -1};

    // --- front-panel knobs and their backlight (FEATURE_KNOBS) ---------------
    // Right knob on input-only pins, which nothing else can use.
    //
    //     GPIO34 <--  S1       GPIO35 <--  S2       GPIO39 (VN) <--  KEY
    //     GPIO13 -->  DIN of the first LED
    //
    // 36 and 39 dip low ~80 ns whenever the ADC powers up: harmless to a 20 ms
    // debounced button, a phantom edge to quadrature, so S1/S2 get 34/35.
    // 34-39 have no internal pull-ups: S1/S2 use the module's 10k; KEY often has
    // none fitted — add 10k KEY to 3V3. Module "5V" pin goes to 3V3 (see C3).
    // 13 is also a ladder leg in env:direct (beside GND/VIN, so the LED chain is
    // one 3-pin plug); KnobService.cpp static_asserts the overlap.
    inline constexpr int8_t PIN_KNOB_S1  = 34;
    inline constexpr int8_t PIN_KNOB_S2  = 35;
    inline constexpr int8_t PIN_KNOB_KEY = 39;
    // Left knob: a second encoder.
    //
    //     GPIO32 <--  S1 (left)     GPIO33 <--  S2 (left)     GPIO36 (VP) <--  KEY (left)
    inline constexpr int8_t PIN_KNOB2_S1  = 32;
    inline constexpr int8_t PIN_KNOB2_S2  = 33;
    // VP has no pull-up: the harness fits external 10k on all three lines of the
    // bare encoder (also stiffer than the 30-80k internal on a metre of wire).
    inline constexpr int8_t PIN_KNOB2_KEY = 36;
    // No pot on this board: analogRead() on a button pin would re-mux it to the ADC.
    inline constexpr int8_t PIN_KNOB_POT = -1;
    inline constexpr int8_t PIN_KNOB_LED = 13;
    inline constexpr bool   KNOB_PULLUPS = false;
    // Knobs on GPIOs, not an I2C expander (see the T-2CAN branch).
    inline constexpr bool    KNOBS_I2C = false;
    inline constexpr int8_t  PCF_BIT_S1  = -1, PCF_BIT_S2  = -1, PCF_BIT_KEY  = -1;
    inline constexpr int8_t  PCF_BIT2_S1 = -1, PCF_BIT2_S2 = -1, PCF_BIT2_KEY = -1;

    // --- the link from board V (FEATURE_LINK) --------------------------------
    // 921600 8N1, named from this board's side:
    //     GPIO26 <--  board V's GPIO21 (its TX)
    //     GPIO27 -->  board V's GPIO20 (its RX)
    inline constexpr int8_t PIN_LINK_RX = 26;
    inline constexpr int8_t PIN_LINK_TX = 27;

#endif

    // Knob/matrix/ladder overlap checks, static_asserted in KnobService.cpp.
    inline constexpr bool isKnobPin(int8_t p) {
        return p >= 0 && (p == PIN_KNOB_S1 || p == PIN_KNOB_S2 || p == PIN_KNOB_KEY ||
                          p == PIN_KNOB2_S1 || p == PIN_KNOB2_S2 || p == PIN_KNOB2_KEY ||
                          p == PIN_KNOB_POT || p == PIN_KNOB_LED);
    }
    inline constexpr bool hasKnob2() {
        return KNOBS_I2C ? (PCF_BIT2_S1 >= 0 && PCF_BIT2_S2 >= 0 && PCF_BIT2_KEY >= 0)
                         : (PIN_KNOB2_S1 >= 0 && PIN_KNOB2_S2 >= 0 && PIN_KNOB2_KEY >= 0);
    }
    inline constexpr bool knobsClashWithMatrix() {
        for (uint8_t i = 0; i < MATRIX_ROWS; ++i) if (isKnobPin(PIN_ROW[i])) return true;
        for (uint8_t i = 0; i < MATRIX_COLS; ++i) if (isKnobPin(PIN_COL[i])) return true;
        return false;
    }
    inline constexpr bool knobsClashWithLadder() {
        for (uint8_t i = 0; i < 5; ++i)
            if (isKnobPin(PIN_RES_CH0[i]) || isKnobPin(PIN_RES_CH1[i])) return true;
        return false;
    }

    inline constexpr const int8_t* resPins(uint8_t ch) {
        return ch == 0 ? PIN_RES_CH0 : PIN_RES_CH1;
    }
    inline constexpr uint8_t resLegs(uint8_t ch) {
        return ch == 0 ? RES_LEGS_CH0 : RES_LEGS_CH1;
    }

    inline constexpr uint32_t HU_UART_BAUD = 38400;

    // Validates the CAN pin settings (Cfg::Settings::canRxPin / canTxPin).
    //   C3     GPIO11-17 SPI flash, 18/19 native USB; 0-10, 20, 21 remain.
    //   ESP32  GPIO6-11 SPI flash; 34-39 input only (CRX yes, CTX never).
    //   S3     19/20 native USB, 26-37 flash + octal PSRAM; the rest is I/O.
    // Only refuses pins that break boot; unbonded pins (20, 24, 28-31 on a
    // WROOM-32) just give a silent bus, fixable from the web page.
    //   T-2CAN The TWAI transceiver (CAN B) is hard-wired to GPIO6/7: no other
    //          pin can ever work, and the rest are the MCP2518FD's SPI and INT
    //          (8, 10-13), QWIIC I2C (1/2), the LED DIN (47) or strapping pins
    //          (0, 3, 45, 46). Only 6 and 7 are accepted, either way round.
#if MEGAOPEN_BOARD_T2CAN
    // Any S3 GPIO this module bonds out (pull-ups everywhere): 0-18, 21, 38-48.
    inline constexpr bool s3Io(int p) {
        return (p >= 0 && p <= 18) || p == 21 || (p >= 38 && p <= 48);
    }
#endif
    inline constexpr bool canPinReadable(int p) {
#if MEGAOPEN_BOARD_C3
        return (p >= 0 && p <= 10) || p == 20 || p == 21;
#elif MEGAOPEN_BOARD_T2CAN
        return p == PIN_CAN_RX || p == PIN_CAN_TX;
#else
        return (p >= 0 && p <= 5) || (p >= 12 && p <= 39);
#endif
    }
    inline constexpr bool canPinDrivable(int p) {
#if MEGAOPEN_BOARD_C3 || MEGAOPEN_BOARD_T2CAN
        return canPinReadable(p);
#else
        // 34-39 have no output driver at all.
        return (p >= 0 && p <= 5) || (p >= 12 && p <= 33);
#endif
    }

    // Per-pin internal pull-up availability (KNOB_PULLUPS is board-wide).
    // ESP32 34-39 have none.
    inline constexpr bool hasPullup(int p) {
#if MEGAOPEN_BOARD_C3
        return p >= 0 && p <= 21;
#elif MEGAOPEN_BOARD_T2CAN
        return s3Io(p);
#else
        return (p >= 0 && p <= 5) || (p >= 12 && p <= 33);
#endif
    }

    // For the boot banner and /api/health.
    inline constexpr const char* NAME =
#if MEGAOPEN_BOARD_C3
        "esp32-c3-supermini";
#elif MEGAOPEN_BOARD_T2CAN
        "lilygo-t2can-s3";
#else
        "esp32-devkit-v1";
#endif
}
