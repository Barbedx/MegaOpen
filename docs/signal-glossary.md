# Signal glossary: where each Vehicle::Sig comes from

**Generated from `docs/signal-glossary.json` by `tools/can_corr.py glossary --md`. Edit the JSON, not this file.**
What this is, the findings and the plan order: `docs/signal-glossary-README.md`.

For OUR Mégane II (X84), whose ECUs the DDT4all scan of 2026-06-14 identified
(`../MeganeCAN/notes/dt4all log`). `pick` is the source to make the default:
`broadcast` decodes a frame the car sends anyway (decode_defaults.inc, all still
DF_HYPOTHESIS), `diag` polls a read on the car bus (MegaOpen does not do this yet),
and `derived` is computed in MegaOpen. Confidence is a judgement until the
correlator (`tools/can_corr.py corr`) has measured it against a capture.

Diag columns: `req` is sent to `tx`, and the reply comes back on `rx`. `byte` is 1-based in the reply, and the 0x61
counts. `shift`/`bits` are LSB-based inside the big-endian word that the field spans
(LE: inside the byte, or byte-swapped), the same convention as decode_defaults.inc.

## ECUs

| ECU file | tx/rx | what |
|---|---|---|
| `S3000_A791_Can_1_X84` | 7E0/7E8 | injection (Sagem S3000) |
| `Climatisation_regulee_Redesign_P0_X84_SERIE` | 744/764 | climate |
| `UCH_84_J84_05_00` | 745/765 | body computer (UCH) |
| `Tdb_BCEKL84_serie_4emeRev` | 743/763 | cluster, approximate match (0905 vs 0812) |
| `Esp_X84_Bosch8.0_F05_Vdiag_0A` | 740/760 | ABS/ESP |
| `UPC_84_(v4.1)` | 74D/76D | fuse box (UPC) |
| `DAE_X84_Serie_V1` | 742/762 | power steering, approximate match |

## Signals

| signal | pick | conf. | broadcast (hypothesis) | diag: ecu req byte/shift/bits | scale → signal |
|---|---|---|---|---|---|
| `SIG_SPEED` | broadcast | medium | `0x354` be16@0 x1 (km/h x100)<br>*hypothesis (x0r)* | 740 `2104` b11>>0/16 *Vitesse véhicule*<br>743 `2110` b10>>0/16 *Vitesse véhicule (km/h) CAN* | (x)/100 Km/H ×100<br>(x)/100 ×100 |
| `SIG_RPM` | broadcast | high | `0x181` be16@0 /8<br>*hypothesis (x0r)* | 7E0 `21A0` b9>>0/16 *Régime moteur*<br>743 `2110` b8>>0/16 *Régime moteur (tr/mn) CAN* | x tr/min<br>(x)/8 |
| `SIG_STEERING` | broadcast | medium | `0x0C2` be16@0 -0x8000 (deg x10)<br>*hypothesis (x0r)* | 740 `2107` b3>>0/16 *Angle volant (Min-720/Max720)* | (x-32767)/10 ° ×10 |
| `SIG_ODOMETER` | broadcast | medium | `0x5FD` be24@0 km<br>*hypothesis (x0r)* | 743 `2195` b3>>0/24 *Odomètre Total* | x Km |
| `SIG_OUT_TEMP` | broadcast | medium | `0x60D` u8@4 -40<br>*hypothesis (x0r)* | 744 `21C1` b4>>0/8 *Température extérieure*<br>745 `2119` b3>>0/8 *Température extérieure (8 bits, °C)* | x-40 °C<br>x |
| `SIG_ENG_TEMP` | broadcast | high | `0x551` u8@0 -40<br>*hypothesis (x0r)* | 744 `21C1` b6>>0/8 *Température d'eau*<br>7E0 `21A0` b4>>0/8 *Température eau mesurée* | x-40 °C<br>x*0.625-40 °C |
| `SIG_DOOR_FL` | broadcast | medium | `0x60D` bit19 of be24@0<br>*hypothesis (x0r)* | 745 `2111` b8>>0/1 LE *PORTE_CONDUCTEUR_OUVERTE* | x |
| `SIG_DOOR_FR` | broadcast | medium | `0x60D` bit20 of be24@0<br>*hypothesis (x0r)* | 745 `2111` b8>>1/1 LE *PORTE_PASSAGER_AVANT_OUVERTE* | x |
| `SIG_DOOR_RL` | broadcast | medium | `0x60D` bit21 of be24@0<br>*hypothesis (x0r)* | 745 `2111` b8>>2/1 LE *PORTE_ARRIERE_GAUCHE_OUVERTE* | x |
| `SIG_DOOR_RR` | broadcast | medium | `0x60D` bit22 of be24@0<br>*hypothesis (x0r)* | 745 `2111` b8>>3/1 LE *PORTE_ARRIERE_DROITE_OUVERTE* | x |
| `SIG_BOOT` | broadcast | medium | `0x60D` bit23 of be24@0<br>*hypothesis (x0r)* | 745 `2111` b8>>4/1 LE *C_HAYON* | x |
| `SIG_REVERSE` | broadcast | low | `0x60D` u8@6 bit4<br>*hypothesis (x0r)* | 745 `2111` b10>>5/1 LE *MARCHE ARRIERE ENGAGEE* | x |
| `SIG_HANDBRAKE` | diag | medium | `0x5C5` u8@0 bit3 or bit2<br>*CONFLICT, left out of decode_defaults* | 743 `2110` b3>>5/1 *Contact frein de parking* | x {0=frein desserré, 1=frein serré} |
| `SIG_BRAKE` | broadcast | low | `0x181` u8@5 bit0<br>*hypothesis (x0r); x0r also names 0x354 last byte bit4* | 740 `2106` b4>>6/2 *Etat de la pédale de frein* | x {0=Relachée, 1=Enfoncée, 3=Inalvide / En défaut} |
| `SIG_KEY_ON` | broadcast | medium | `0x60D` bit10 of be24@0<br>*hypothesis (x0r)* | 743 `2110` b4>>4/1 *APC (CAN)*<br>745 `21AA` b21>>5/1 LE *+APC_84* | x {0=APC absent, 1=APC présent}<br>x {0=APC OFF, 1=APC ON} |
| `SIG_KEY_ACC` | broadcast | low | `0x60D` bit9 of be24@0<br>*hypothesis (x0r)* | 745 `2182` b13>>0/8 LE *Etat ACC sur réseau MUX* | x (derived) |
| `SIG_BACKLIGHT` | broadcast | low | `0x645` u8@1, 0xFF = off<br>*hypothesis (x0r)* | 744 `21C2` b8>>0/8 *Niveau éclairage Nuit* | x*0.4 % (derived) |
| `SIG_LIGHT_POS` | broadcast | medium | `0x60D` bit18 of be24@0<br>*hypothesis (x0r)* | 743 `2110` b4>>3/1 *+Lanterne (CAN)*<br>745 `2111` b5>>1/1 LE *LANTERNE* | x {0=Absent, 1=Présent}<br>x |
| `SIG_LIGHT_DIPPED` | broadcast | medium | `0x60D` bit17 of be24@0<br>*hypothesis (x0r)* | 743 `2110` b4>>2/1 *Feux croisement (CAN)* | x {0=Absent, 1=Présent} |
| `SIG_LIGHT_MAIN` | broadcast | medium | `0x60D` bit11 of be24@0<br>*hypothesis (x0r)* | 743 `2110` b4>>1/1 *Feux route (CAN)* | x {0=Absent, 1=Présent} |
| `SIG_FOG_FRONT` | broadcast | medium | `0x60D` bit8 of be24@0<br>*hypothesis (x0r)* | 743 `2110` b4>>0/1 *Feux brouillard AV (CAN)*<br>745 `2111` b5>>5/1 LE *BROUILLARD_AV* | x {0=Absent, 1=Présent}<br>x |
| `SIG_FOG_REAR` | broadcast | medium | `0x60D` bit2 of be24@0<br>*hypothesis (x0r)* | 743 `2110` b5>>7/1 *Feux brouillard AR (CAN)*<br>745 `2111` b5>>6/1 LE *BROUILLARD_AR* | x {0=Absent, 1=Présent}<br>x |
| `SIG_IND_LEFT` | broadcast | medium | `0x60D` bit13 of be24@0, 500 ms latch<br>*hypothesis (x0r)* | 745 `2111` b6>>0/1 LE *CLIGNOTANT G* | x |
| `SIG_IND_RIGHT` | broadcast | medium | `0x60D` bit14 of be24@0, 500 ms latch<br>*hypothesis (x0r)* | 745 `2111` b6>>1/1 LE *CLIGNOTANT D* | x |
| `SIG_TRIP1` | diag | medium | — | 743 `2195` b8>>0/24 *Odomètre Partiel* | (x)/10 Km ×10 |
| `SIG_FUEL_INST` | derived | medium | `0x551` u8@1 fuel used, 80 mm3/LSB (12 500 = 1 L)<br>*hypothesis (x0r), encoding corroborated by the cluster* | 7E0 `21A9` b3>>0/16 *Consommation carburant instantanée* | (x)/100 L/H (derived) |
| `SIG_RANGE` | derived | low | — | — | — |
| `SIG_AVG_SPEED` | derived | low | — | — | — |
| `SIG_FUEL_AVG` | derived | low | — | — | — |
| `SIG_TRIP_ELAPSED` | derived | high | — | — | — |
| `SIG_AC_ON` | diag | medium | — | 744 `21C2` b17>>6/1 *Demande AC* | x {0=OFF, 1=ON} |
| `SIG_AC_COMPRESSOR` | diag | medium | — | 744 `21C2` b6>>4/2 *Recopie de l'état du compresseur* | x {0=OFF, 1=ON, 3=indisponible} |
| `SIG_AC_RECIRC` | diag | medium | — | 744 `21C2` b21>>4/2 *Recyclage* | x {0=air externe, 1=recy, 2=volet en mouvement, 3=indisponible} |
| `SIG_AC_FAN` | diag | low | — | 744 `21C2` b15>>0/8 *Consigne PWM GMV habitacle*<br>744 `21C2` b21>>6/2 *Etat du ventilateur habitacle* | x*0.5 % (derived)<br>x {0=OFF, 1=ON, 3= indisponible} (derived) |
| `SIG_AC_VENT_FACE` | diag | low | — | 744 `21C2` b19>>0/8 *Position volet distribution* | x % (derived) |
| `SIG_AC_TEMP_DRV` | none | low | — | 744 `21C2` b16>>0/8 *Consigne de température air soufflé* | x-40 °C (derived) |
| `SIG_AC_DEFOG` | diag | low | — | 744 `21C2` b4>>6/2 *Etat de demande LARC manuel* | x {0=OFF, 1=ON, 3=indisponible} |
| `SIG_TPMS_FL` | diag | low | — | 745 `2166` b3>>0/8 *SSPP Pression pneu mesuree roue 1* | (x*3500)/255 (derived) |

## Notes

- **SIG_SPEED**: The cluster reports the speed it RECEIVES over CAN as a 16-bit /100, the same encoding as x0r's 0x354.
- **SIG_RPM**: The cluster's received-over-CAN rpm is 16-bit /8, exactly x0r's 0x181. Logged 2026-06-14 at idle: 747 rpm.
- **SIG_STEERING**: ESP's diag angle is (raw-32767)/10. The broadcast bias may be 0x7FFF, not 0x8000: one LSB, 0.1 deg.
- **SIG_OUT_TEMP**: The climate unit's copy is u8-40, the same as x0r. The UCH owns the sensor.
- **SIG_ENG_TEMP**: The climate unit receives the coolant temperature from the ECM as u8-40 (x0r's encoding). The ECM's own diag value is 0.625/LSB, an internal scale.
- **SIG_DOOR_FL**: The 'conducteur' door is FL on a left-hand-drive car.
- **SIG_REVERSE**: x0r calls this bit unclear. If the capture does not confirm it, the UCH diag is the source.
- **SIG_HANDBRAKE**: The cluster reads the switch itself. The correlator settles 0x5C5's bit against this one.
- **SIG_BRAKE**: ESP value 3 means invalid or faulty, not pressed.
- **SIG_KEY_ON**: With the key off, the ECUs do not answer diagnostics at all.
- **SIG_KEY_ACC**: The UCH item is a raw byte with no published values.
- **SIG_BACKLIGHT**: The climate unit's received copy is 0.4 %/LSB (0..250 = 100 %), which does not fit x0r's 0..0xFE. Settle it from a capture.
- **SIG_IND_LEFT**: Blink pulses: too fast for polling, so the broadcast is the only real source.
- **SIG_TRIP1**: The cluster's own trip, already in 0.1 km. No broadcast is known.
- **SIG_FUEL_INST**: Derive L/100 km x10 = L/h / (km/h) x 1000, from the 0x551 fuel counter's rate or S3000 21A9, plus the speed. The cluster's 2110 'carburant consommé (mm3) CAN' x80 confirms the counter's unit.
- **SIG_RANGE**: Not in any definition. Compute it in MegaOpen from the fuel level and the average consumption. The fuel level source is still open.
- **SIG_AVG_SPEED**: Computed in MegaOpen from SPEED over the trip.
- **SIG_FUEL_AVG**: Computed in MegaOpen from the 0x551 fuel counter over the trip distance.
- **SIG_TRIP_ELAPSED**: MegaOpen's own clock since key-on.
- **SIG_AC_ON**: The climate panel may talk to the HVAC unit directly, off the CAN bus. Check the capture first: if nothing moves when the panel does, poll.
- **SIG_AC_COMPRESSOR**: 3 = unavailable, so map it to 0. It is a copy of something the ECM broadcasts, so a broadcast row probably exists.
- **SIG_AC_RECIRC**: 0 = outside air, 1 = recirculation, 2 = flap moving (hold the last value), 3 = unavailable.
- **SIG_AC_FAN**: PWM % into 0-7 steps, with the thresholds measured per panel position. The fan state gates 0.
- **SIG_AC_VENT_FACE**: Distribution flap % into face/floor/up ranges, measured per panel position. The same item feeds VENT_FLOOR and VENT_UP.
- **SIG_AC_TEMP_DRV**: Only the BLOWN-AIR setpoint exists, not the temperature the driver chose. The blend flap ('Position volet mixage') is the nearest proxy.
- **SIG_AC_DEFOG**: LARC is the heated REAR window. Whether the RAV4 bit means that is open (raise-protocol.md).
- **SIG_TPMS_FL**: Only if the car has TPMS (SSPP). The value is mbar = raw x 3500/255. Which wheel is 'roue 1'..4 is unknown, as is the encoding of SIG_TPMS_*. Wheels 2-4 sit at bytes 9/15/21.

## Not on this car

`SIG_AC_AUTO_HI`, `SIG_AC_AUTO_LO`, `SIG_AC_DUAL`, `SIG_AC_MAX_FRONT`, `SIG_AC_REAR`, `SIG_AC_AQS`, `SIG_AC_ECO`, `SIG_SEAT_*`, `SIG_RADAR_* (no parking aid found in the 2026-06-14 scan)`, `SIG_TRIP2`
