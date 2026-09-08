# FloodMesh — 5 × 7 cm perfboard routing guide

> **Superseded for the user's actual build:** use [18 × 24-hole build manual](perfboard-18x24-build.md), which mounts the Heltec, microphone and amplifier on the same perfboard. The carrier arrangement below is retained as an earlier remote-module alternative; its assumed 20 × 28 hole count is not the user's board.

This is a **socketed carrier** for the Heltec WiFi LoRa 32 V3/V3.2. It fits a
standard 20 × 28-hole, 2.54 mm isolated-pad perfboard (nominal 50 × 70 mm).
The Heltec remains removable; the microphone, amplifier/speaker and buttons
are external harnesses. Trying to mount all three breakout boards and five
switches on the same 5 × 7 cm surface makes the LoRa antenna, audio wiring and
future repair needlessly unreliable.

## Pin decisions used by this guide

`PIN_MIC_SD` is moved from GPIO45 to GPIO40 and `PIN_AMP_SD` is assigned to
GPIO39 in `include/floodmesh_pins.h`. GPIO45 and GPIO46 are boot-strapping
pins, so they must not be driven by the microphone during reset. GPIO39–42 are
the ESP32-S3 JTAG pads; the project is already using GPIO41/42 for I2S, so this
carrier deliberately gives up external JTAG while audio is fitted.

| Function | Heltec header / pin | Carrier net | Remote end |
|---|---:|---|---|
| Common ground | J2-1 and J3-1 | `GND` | Every harness |
| 3.3 V supply | J3-2 or J3-3 | `3V3` | Mic, amp, buzzer |
| Button UP / SAFE | J3-15 / GPIO4 | `BTN_UP` | Panel switch |
| Button DOWN / MEDICAL | J3-16 / GPIO5 | `BTN_DOWN` | Panel switch |
| Button LEFT / WATER | J3-17 / GPIO6 | `BTN_LEFT` | Panel switch |
| Button RIGHT / EVACUATE | J3-18 / GPIO7 | `BTN_RIGHT` | Panel switch |
| PTT | J2-13 / GPIO47 | `BTN_PTT` | Panel switch |
| Buzzer drive | J3-13 / GPIO2 | `BUZZ` | Q1 base resistor |
| Mic BCLK | J3-7 / GPIO42 | `MIC_SCK` | INMP441 SCK |
| Mic LRCLK / WS | J3-8 / GPIO41 | `MIC_WS` | INMP441 WS |
| Mic data | J3-9 / GPIO40 | `MIC_SD` | INMP441 SD |
| Amp LRCLK | J2-18 / GPIO19 | `AMP_LRC` | MAX98357A LRC |
| Amp BCLK | J2-17 / GPIO20 | `AMP_BCLK` | MAX98357A BCLK |
| Amp data | J2-14 / GPIO48 | `AMP_DIN` | MAX98357A DIN |
| Amp shutdown | J3-10 / GPIO39 | `AMP_SD` | MAX98357A SD |

Header pin 1 is the end nearest the USB connector on both headers. Use the
labels on the Heltec PCB and its pin-map image to verify before soldering. Fit
the two socket strips *on the Heltec first*, press them into the perfboard as a
jig, then solder. Do not place the 2.54 mm strips from an assumed row spacing.

## Top-side placement

Orient the carrier with its 70 mm dimension vertical. Keep the Heltec's U.FL
antenna end at the top edge and its USB connector at the lower end. Put the
board in the upper 50 mm, centered left-to-right; it will occupy roughly rows
1–21. Use two 1 × 18 **female machine-pin** socket strips, so all underside
routing is complete and inspected before the Heltec is plugged in.

Place three keyed 1 × 6 headers across the last four rows:

| Header, left-to-right pin order | Purpose |
|---|---|
| `J_BTN`: `GND, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_PTT` | Six-core cable to the five panel switches. Each switch connects its signal to the shared ground. |
| `J_MIC`: `GND, 3V3, MIC_SCK, MIC_WS, MIC_SD, GND_LR` | INMP441 harness. Tie the mic's `L/R` pad to `GND_LR` at the microphone end. |
| `J_AMP`: `GND, 3V3, AMP_DIN, AMP_BCLK, AMP_LRC, AMP_SD` | MAX98357A harness. Speaker leads stay at the amplifier module. |

Use keyed JST-XH, Molex KK, or latching 0.1-inch housings rather than loose
Dupont leads. Mark pin 1 on the perfboard with a square pad, paint dot, or
notched shroud.

Mount the buzzer on the left or right edge, or put a two-pin `J_BUZ` header
there if it belongs in the enclosure lid. Keep its sound opening unobstructed.
Do **not** add the battery holder to this perfboard: use the Heltec's battery
connector and keep the protected one-cell pack separately restrained in the
enclosure.

## Underside tracks

All coordinates below refer to the component-side orientation above.

1. Along the free area just below the Heltec, solder a `GND` bus and a `3V3`
   bus running horizontally across the board. Use 22–24 AWG tinned copper for
   both. Feed `GND` from **both** J2-1 and J3-1; feed `3V3` from J3-2 and/or
   J3-3. Do not use the Heltec `5V` pin for any battery-powered load.
2. Branch the ground bus to pin 1 of all three harness headers and to Q1's
   emitter. Branch the 3.3 V bus separately to J_MIC-2, J_AMP-2 and the
   buzzer positive terminal. This is a star-ish supply layout; it prevents
   speaker/buzzer current from sharing the microphone return.
3. Route every logic signal point-to-point with insulated 30 AWG wire-wrap
   wire. Keep `MIC_SCK`, `MIC_WS`, `MIC_SD`, `AMP_BCLK`, `AMP_LRC` and
   `AMP_DIN` short and away from the buzzer and speaker cables. Route a ground
   wire beside each audio harness; a short twisted pair for `SCK+GND` and
   `BCLK+GND` is worthwhile.
4. Build the buzzer switch under the Heltec or along an edge: `GPIO2 → 1 kΩ
   (R1) → Q1 base`; `10 kΩ (R2)` from base to emitter; emitter to `GND`;
   collector to buzzer negative; buzzer positive to `3V3`. Q1 may be a
   2N2222/PN2222A. Use a **3.3 V active buzzer**. A flyback diode is needed
   only if the chosen sounder is magnetic/coil-based, not for a piezo buzzer.
5. Place a 100 nF ceramic capacitor directly across `VDD/GND` at the INMP441
   end. At the MAX98357A end place 100 nF ceramic and 47–100 µF electrolytic
   across `VIN/GND`. Keep the amplifier's `SPK+` and `SPK−` as a twisted pair
   to the speaker; neither lead may touch ground.

Use wire, not long beads of solder, for every track. Bare wire is appropriate
only for the two supply buses; every signal must have insulation. Add a small
label beside each Heltec socket pad before the board is plugged in.

### Signal strap list

Solder these insulated point-to-point straps on the underside. The `J_*` pin
numbers are in the left-to-right order printed in the placement table.

| From Heltec socket pad | To carrier point | Net |
|---|---|---|
| J3-15 / GPIO4 | J_BTN-2 | `BTN_UP` |
| J3-16 / GPIO5 | J_BTN-3 | `BTN_DOWN` |
| J3-17 / GPIO6 | J_BTN-4 | `BTN_LEFT` |
| J3-18 / GPIO7 | J_BTN-5 | `BTN_RIGHT` |
| J2-13 / GPIO47 | J_BTN-6 | `BTN_PTT` |
| J3-13 / GPIO2 | R1 (1 kΩ) input | `BUZZ` |
| J3-7 / GPIO42 | J_MIC-3 | `MIC_SCK` |
| J3-8 / GPIO41 | J_MIC-4 | `MIC_WS` |
| J3-9 / GPIO40 | J_MIC-5 | `MIC_SD` |
| J2-18 / GPIO19 | J_AMP-5 | `AMP_LRC` |
| J2-17 / GPIO20 | J_AMP-4 | `AMP_BCLK` |
| J2-14 / GPIO48 | J_AMP-3 | `AMP_DIN` |
| J3-10 / GPIO39 | J_AMP-6 | `AMP_SD` |

Connect J_BTN-1, J_MIC-1, J_MIC-6 and J_AMP-1 to the ground bus. Connect
J_MIC-2 and J_AMP-2 to the 3.3 V bus. R1's other end reaches Q1's base; R2
connects from that base pad to the ground/emitter pad. Verify the E/B/C order
for the particular 2N2222-family package before soldering it—TO-92 pinouts
are not universal.

## Harness details and limits

- All five switches are normally open and use the firmware's internal pull-ups:
  one terminal to its signal, the other to the common ground. No external pull
  resistors are needed. Keep the button harness under about 30 cm or add 100 nF
  from each signal to ground at the carrier if long-cable noise appears.
- The INMP441 and MAX98357A are 3.3 V peripherals here. Do not power the amp
  from the USB-only 5 V header. A 3.3 V amp works, but its maximum speaker
  loudness is lower than at 5 V.
- The 3.3 V header is specified by Heltec for up to 500 mA. Its load must cover
  the board, mic, amplifier peaks and buzzer. Use an 8 Ω speaker and a
  low-current buzzer; if the amplifier browns the board out during LoRa TX,
  move the amp to a separately regulated 3.3 V rail with common ground.
- Keep the U.FL antenna and its coax above the top edge, at least 15 mm from
  the amplifier, speaker leads, buzzer and large ground loops. Do not run a
  supply bus around the antenna end of the carrier.

## Soldering and bring-up order

1. Fit and solder the empty Heltec sockets; verify their pad spacing with the
   unplugged Heltec. Then solder and label the two buses and three harness
   headers.
2. With the Heltec still unplugged, verify there is no continuity between `3V3`
   and `GND`, no short between any two signal nets, and each button signal only
   reaches ground while that switch is pressed.
3. Fit Q1/R1/R2 and the 3.3 V buzzer. Plug in the Heltec and run the current
   Milestone 1 build. Confirm boot chime, each navigation action, PTT, and
   every PTT+button alarm chord before attaching either audio module.
4. Attach the microphone next. Power-cycle at least twenty times; this is the
   practical check that the new GPIO40 data route is not disturbing reset.
5. Attach the amplifier and speaker last, initially at low volume. Confirm its
   shutdown line is low when idle before treating audio power draw as final.

The V3/V3.2 header numbering, 3.3 V/5 V limits, dimensions, and its onboard
battery connector come from Heltec's hardware datasheet and pin map. The
vendor also recommends avoiding boot-strapping pins and on-board peripheral
pins for external hardware.
