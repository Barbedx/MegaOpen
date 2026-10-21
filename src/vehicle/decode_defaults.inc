// The compiled decode table: CanDecode's factory defaults. Included inside an
// array in CanDecode.cpp; one row per signal, SORTED BY ID (load() refuses an
// unsorted table, and test_decode checks this one).
//
// Sources: docs/megane-can-ids.md (the x0r Clio III decode), and Renault's X84
// broadcast matrix (DDT2000 v84.05), which agrees with every 0x60D row here.
// Its convention, `bN.O/W`: byte N counted from 1, bit offset O counted FROM THE
// MSB, width W. As a 1-byte row: startByte N-1, shift 8-O-W, mask (1<<W)-1.
//
// DF_HYPOTHESIS = not measured on our Mégane II yet. The owner confirmed doors,
// boot, lights and indicators in the car (2026-10-20): those rows are measured.
// A 2-bit Renault state (0 no, 1 yes, 2-3 reserved/not fitted/invalid) that is
// a yes/no reads the whole field with DF_STATE2: 2 and 3 leave the signal as
// it was, so an invalid frame never reads as "yes" (reverse, brake...). Where
// the matrix gives every value a meaning (wipers, MIL, roof), the row is raw.
// DF_STATE2_INV where the matrix's 00 is the yes (belts: contact open when
// buckled; the seat: 00 = passenger detected): those signals read 1 = yes.
//
// Left out on purpose:
//   0x60D b5         a second engine temperature; 0x551 b0 is the one used.
//
//  id      start n  shift mask        mul div add       signal              flags                      holdMs
{ 0x0C2,   0, 2,  0, 0xFFFFu,     1, 1, -32767, SIG_STEERING,   DF_HYPOTHESIS,               0 },  // b1.0/16: 0.1 deg/LSB, -3276.7, so 0x7FFF is centre
{ 0x181,   0, 2,  0, 0xFFFFu,     1, 8,      0, SIG_RPM,        DF_HYPOTHESIS,               0 },  // raw / 8
{ 0x181,   3, 1,  0, 0xFFu,     423, 100,  -68, SIG_THROTTLE,   DF_HYPOTHESIS,               0 },  // pedal: % = raw x 0.423 - 6.768; 0x10 released
{ 0x181,   5, 1,  0, 0x3u,        1, 1,      0, SIG_BRAKE,      DF_HYPOTHESIS | DF_STATE2,   0 },  // b6.6/2
{ 0x181,   5, 1,  3, 0x3u,        1, 1,      0, SIG_CLUTCH,     DF_HYPOTHESIS | DF_STATE2,   0 },  // b6.3/2
{ 0x215,   1, 1,  4, 0x3u,        1, 1,      0, SIG_NEUTRAL,    DF_HYPOTHESIS | DF_STATE2,   0 },  // b2.2/2
{ 0x215,   1, 1,  6, 0x3u,        1, 1,      0, SIG_REVERSE_UPC, DF_HYPOTHESIS | DF_STATE2,  0 },  // b2.0/2
{ 0x2A0,   0, 1,  0, 0xFFu,       5, 1,  -1000, SIG_ACCEL_LONG, DF_HYPOTHESIS,               0 },  // 0.05 m/s2, -10
{ 0x2A0,   1, 2,  0, 0xFFFFu,     4, 1, -131068, SIG_ACCEL_LAT, DF_HYPOTHESIS,               0 },  // 0.004 g, -131.068
{ 0x2A0,   3, 2,  0, 0xFFFFu,     2, 1, -65534, SIG_YAW_RATE,   DF_HYPOTHESIS,               0 },  // 0.2 deg/s, -6553.4
{ 0x354,   0, 2,  0, 0xFFFFu,     1, 1,      0, SIG_SPEED,      DF_HYPOTHESIS,               0 },  // km/h x100, as 0x7D 03 wants it
{ 0x35D,   0, 1,  0, 0x1u,        1, 1,      0, SIG_AC_REQUEST, DF_HYPOTHESIS,               0 },  // b1.7/1
{ 0x35D,   0, 1,  1, 0x3u,        1, 1,      0, SIG_REAR_DEFROST_REQ, DF_HYPOTHESIS,         0 },  // b1.5/2
{ 0x35D,   2, 1,  5, 0x7u,        1, 1,      0, SIG_WIPERS,     DF_HYPOTHESIS,               0 },  // b3.0/3
{ 0x35D,   4, 1,  6, 0x1u,        1, 1,      0, SIG_ROLLING,    DF_HYPOTHESIS,               0 },  // b5.1/1
{ 0x35D,   5, 1,  0, 0xFFu,    1000, 1,      0, SIG_SUN_LUX,    DF_HYPOTHESIS,               0 },  // 1000 lux/LSB
{ 0x374,   0, 1,  0, 0xFFu,       1, 1,    -40, SIG_CLIM_AIR_SET, DF_HYPOTHESIS,             0 },  // blown air, raw - 40
{ 0x374,   1, 1,  4, 0x1u,        1, 1,      0, SIG_CLIM_AC_SEL, DF_HYPOTHESIS,              0 },  // b2.3/1
{ 0x374,   1, 1,  5, 0x3u,        1, 1,      0, SIG_CLIM_FAN_STATE, DF_HYPOTHESIS,           0 },  // b2.1/2
{ 0x374,   2, 1,  0, 0xFFu,       5, 1,   -400, SIG_CLIM_EVAP,  DF_HYPOTHESIS,               0 },  // 0.5 C, -40
{ 0x551,   0, 1,  0, 0xFFu,       1, 1,    -40, SIG_ENG_TEMP,   DF_HYPOTHESIS,               0 },  // coolant, raw - 40
{ 0x551,   1, 1,  0, 0xFFu,       1, 1,      0, SIG_FUEL_COUNTER, DF_HYPOTHESIS,             0 },  // 80 mm3 a count, wraps
{ 0x5C5,   0, 1,  0, 0x3u,        1, 1,      0, SIG_FUEL_GAUGE_DIAG, DF_HYPOTHESIS,          0 },  // b1.6/2
{ 0x5C5,   0, 1,  2, 0x1u,        1, 1,      0, SIG_HANDBRAKE,  DF_HYPOTHESIS,               0 },  // b1.5/1: bit2, settles x0r's bit3
{ 0x5C5,   0, 1,  5, 0x1u,        1, 1,      0, SIG_FUEL_LOW,   DF_HYPOTHESIS,               0 },  // b1.2/1
{ 0x5C5,   1, 3,  0, 0xFFFFFFu,   1, 1,      0, SIG_ODO_CLUSTER, DF_HYPOTHESIS,              0 },  // km
{ 0x5C5,   7, 1,  0, 0x7Fu,       1, 1,      0, SIG_FUEL_LEVEL, DF_HYPOTHESIS,               0 },  // b8.1/7 litres
{ 0x5FD,   0, 3,  0, 0xFFFFFFu,   1, 1,      0, SIG_ODOMETER,   DF_HYPOTHESIS,               0 },  // km; not in the X84 matrix
// 0x60D b0-2: bits numbered inside the 24-bit big-endian word.
{ 0x60D,   0, 3,  2, 0x1u,        1, 1,      0, SIG_FOG_REAR,   DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3,  8, 0x1u,        1, 1,      0, SIG_FOG_FRONT,  DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3,  9, 0x1u,        1, 1,      0, SIG_KEY_ACC,    DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3, 10, 0x1u,        1, 1,      0, SIG_KEY_ON,     DF_HYPOTHESIS,               0 },
{ 0x60D,   0, 3, 11, 0x1u,        1, 1,      0, SIG_LIGHT_MAIN, 0,                           0 },
{ 0x60D,   0, 3, 13, 0x1u,        1, 1,      0, SIG_IND_LEFT,   DF_LATCH,                  500 },  // blink pulses: 500 ms latch
{ 0x60D,   0, 3, 14, 0x1u,        1, 1,      0, SIG_IND_RIGHT,  DF_LATCH,                  500 },
{ 0x60D,   0, 3, 17, 0x1u,        1, 1,      0, SIG_LIGHT_DIPPED, 0,                         0 },
{ 0x60D,   0, 3, 18, 0x1u,        1, 1,      0, SIG_LIGHT_POS,  0,                           0 },
{ 0x60D,   0, 3, 19, 0x1u,        1, 1,      0, SIG_DOOR_FL,    0,                           0 },
{ 0x60D,   0, 3, 20, 0x1u,        1, 1,      0, SIG_DOOR_FR,    0,                           0 },
{ 0x60D,   0, 3, 21, 0x1u,        1, 1,      0, SIG_DOOR_RL,    0,                           0 },
{ 0x60D,   0, 3, 22, 0x1u,        1, 1,      0, SIG_DOOR_RR,    0,                           0 },
{ 0x60D,   0, 3, 23, 0x1u,        1, 1,      0, SIG_BOOT,       0,                           0 },
{ 0x60D,   2, 1,  3, 0x3u,        1, 1,      0, SIG_LOCKED,     DF_HYPOTHESIS,               0 },  // b3.3/2
{ 0x60D,   4, 1,  0, 0xFFu,       1, 1,    -40, SIG_OUT_TEMP,   DF_HYPOTHESIS,               0 },  // raw - 40
{ 0x60D,   6, 1,  4, 0x3u,        1, 1,      0, SIG_REVERSE,    DF_HYPOTHESIS | DF_STATE2,   0 },  // b7.2/2
{ 0x625,   0, 1,  0, 0x1u,        1, 1,      0, SIG_REAR_DEFROST, DF_HYPOTHESIS,             0 },  // b1.7/1
{ 0x625,   1, 1,  7, 0x1u,        1, 1,      0, SIG_AC_COMP_UPC, DF_HYPOTHESIS,              0 },  // b2.0/1
{ 0x625,   2, 1,  0, 0xFFu,      25, 4,      0, SIG_BATT_V,     DF_HYPOTHESIS,               0 },  // 0.0625 V/LSB -> V x100
{ 0x625,   3, 1,  5, 0x1u,        1, 1,      0, SIG_BATT_LAMP,  DF_HYPOTHESIS,               0 },  // b4.2/1
{ 0x625,   3, 1,  7, 0x1u,        1, 1,      0, SIG_OIL_PRESS_SW, DF_HYPOTHESIS,             0 },  // b4.0/1
{ 0x645,   1, 1,  0, 0xFFu,       1, 1,      0, SIG_BACKLIGHT,  DF_HYPOTHESIS,               0 },  // 0x00-0xFE, 0xFF = off
{ 0x651,   0, 1,  0, 0x1u,        1, 1,      0, SIG_AIRBAG_FAULT, DF_HYPOTHESIS,             0 },  // b1.7/1
{ 0x651,   0, 1,  1, 0x1u,        1, 1,      0, SIG_PASS_AIRBAG_OFF, DF_HYPOTHESIS,          0 },  // b1.6/1
{ 0x651,   1, 1,  0, 0x3u,        1, 1,      0, SIG_BELT_DRV,   DF_HYPOTHESIS | DF_STATE2 | DF_STATE2_INV, 0 },  // b2.6/2
{ 0x651,   1, 1,  4, 0x3u,        1, 1,      0, SIG_PASS_PRESENT, DF_HYPOTHESIS | DF_STATE2 | DF_STATE2_INV, 0 },  // b2.2/2
{ 0x651,   1, 1,  6, 0x3u,        1, 1,      0, SIG_BELT_PASS,  DF_HYPOTHESIS | DF_STATE2 | DF_STATE2_INV, 0 },  // b2.0/2
{ 0x684,   0, 1,  3, 0x1u,        1, 1,      0, SIG_ROOF_FAULT, DF_HYPOTHESIS,               0 },  // b1.4/1
{ 0x684,   0, 1,  5, 0x7u,        1, 1,      0, SIG_ROOF_POS,   DF_HYPOTHESIS,               0 },  // b1.0/3
{ 0x715,   3, 1,  2, 0xFu,        1, 1,      0, SIG_OIL_LEVEL,  DF_HYPOTHESIS,               0 },  // b4.2/4
{ 0x715,   3, 1,  6, 0x3u,        1, 1,      0, SIG_MIL,        DF_HYPOTHESIS,               0 },  // b4.0/2
{ 0x715,   4, 1,  1, 0x7Fu,       1, 1,      0, SIG_FUEL_LEVEL_APV, DF_HYPOTHESIS,           0 },  // b5.0/7 litres
{ 0x715,   5, 1,  0, 0xFFu,     250, 1,      0, SIG_SERVICE_KM, DF_HYPOTHESIS,               0 },  // 250 km/LSB
