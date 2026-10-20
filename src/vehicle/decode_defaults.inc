// The compiled decode table: CanDecode's factory defaults. Included inside an
// array in CanDecode.cpp; one row per signal, SORTED BY ID (load() refuses an
// unsorted table, and test_decode checks this one).
//
// Source: docs/megane-can-ids.md, cross-checked with docs/raise-protocol.md §6.
// EVERY ROW IS A HYPOTHESIS (DF_HYPOTHESIS): the ids and layouts are the Clio III
// decode from x0r.fr/blog/39, same platform generation, never measured on our
// Mégane II. The emitter stays off until the table is verified in the car.
//
// Left out on purpose:
//   0x5C5 handbrake  the bit is in CONFLICT (bit3 vs bit2), and raise-protocol.md
//                    §8 says not to emit handbrake at all for now.
//   0x60D b5         a second engine temperature; 0x551 b0 is the one used.
//
//  id      start n  shift mask        mul div add     signal           flags                      holdMs
{ 0x0C2,   0, 2,  0, 0xFFFFu,     1, 1, -32768, SIG_STEERING,   DF_HYPOTHESIS,               0 },  // 0.1 deg/LSB, 0x8000 bias
{ 0x181,   0, 2,  0, 0xFFFFu,     1, 8,      0, SIG_RPM,        DF_HYPOTHESIS,               0 },  // raw / 8
{ 0x181,   5, 1,  0, 0x1u,        1, 1,      0, SIG_BRAKE,      DF_HYPOTHESIS,               0 },  // b5 bit0
{ 0x354,   0, 2,  0, 0xFFFFu,     1, 1,      0, SIG_SPEED,      DF_HYPOTHESIS,               0 },  // km/h x100, as 0x7D 03 wants it
{ 0x551,   0, 1,  0, 0xFFu,       1, 1,    -40, SIG_ENG_TEMP,   DF_HYPOTHESIS,               0 },  // coolant, raw - 40
{ 0x5FD,   0, 3,  0, 0xFFFFFFu,   1, 1,      0, SIG_ODOMETER,   DF_HYPOTHESIS,               0 },  // km
// 0x60D b0-2: bits numbered inside the 24-bit big-endian word.
{ 0x60D,   0, 3,  2, 0x1u,        1, 1,      0, SIG_FOG_REAR,   DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3,  8, 0x1u,        1, 1,      0, SIG_FOG_FRONT,  DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3,  9, 0x1u,        1, 1,      0, SIG_KEY_ACC,    DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3, 10, 0x1u,        1, 1,      0, SIG_KEY_ON,     DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3, 11, 0x1u,        1, 1,      0, SIG_LIGHT_MAIN, DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3, 13, 0x1u,        1, 1,      0, SIG_IND_LEFT,   DF_HYPOTHESIS | DF_LATCH,  500 },  // blink pulses: 500 ms latch
{ 0x60D,   0, 3, 14, 0x1u,        1, 1,      0, SIG_IND_RIGHT,  DF_HYPOTHESIS | DF_LATCH,  500 },
{ 0x60D,   0, 3, 17, 0x1u,        1, 1,      0, SIG_LIGHT_DIPPED, DF_HYPOTHESIS,             0 },
{ 0x60D,   0, 3, 18, 0x1u,        1, 1,      0, SIG_LIGHT_POS,  DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3, 19, 0x1u,        1, 1,      0, SIG_DOOR_FL,    DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3, 20, 0x1u,        1, 1,      0, SIG_DOOR_FR,    DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3, 21, 0x1u,        1, 1,      0, SIG_DOOR_RL,    DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3, 22, 0x1u,        1, 1,      0, SIG_DOOR_RR,    DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3, 23, 0x1u,        1, 1,      0, SIG_BOOT,       DF_HYPOTHESIS,               0 },
{ 0x60D,   4, 1,  0, 0xFFu,       1, 1,    -40, SIG_OUT_TEMP,   DF_HYPOTHESIS,               0 },  // raw - 40
{ 0x60D,   6, 1,  4, 0x1u,        1, 1,      0, SIG_REVERSE,    DF_HYPOTHESIS,               0 },  // b6 bit4
{ 0x645,   1, 1,  0, 0xFFu,       1, 1,      0, SIG_BACKLIGHT,  DF_HYPOTHESIS,               0 },  // 0x00-0xFE, 0xFF = off
