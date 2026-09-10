# FloodMesh — as-built wiring

**This is the authoritative connection list.** It matches board 1 as physically
soldered and verified working on 9 September 2026, and matches
`include/floodmesh_pins.h`, which is what the firmware compiles against.

Where `docs/perfboard-routing.md` or `docs/perfboard-18x24-build.md` disagree,
**this document and the pin header win** — see the deviations section at the end.

---

## 1. Onboard — nothing to wire

These live on the Heltec module itself. Listed so nobody wires them by mistake.

| Function | GPIO |
|---|---|
| OLED SDA / SCL / RST | 17 / 18 / 21 (I²C address `0x3C`) |
| SX1262 NSS / SCK / MOSI / MISO | 8 / 9 / 10 / 11 |
| SX1262 RST / BUSY / DIO1 | 12 / 13 / 14 |
| Vext control | 36 (active LOW) |
| Battery sense ADC | 1 |
| Battery divider gate | 37 (**active HIGH on V3.2**) |
| White LED | 35 |
| PRG button | 0 |

---

## 2. Keypad — 2×2 matrix, four wires, no ground

The module has four pads and **no ground pin**. Each key bridges one column to
one row; the firmware scans by driving a row low and reading the columns.

| Module pad | GPIO | Role |
|---|---|---|
| L1 | 7 | column |
| L2 | 6 | column |
| R1 | 5 | row |
| R2 | 4 | row |

Key positions, measured on the assembled hardware:

| Key | Bridges | PTT up | PTT held |
|---|---|---|---|
| top-left | L1 × R1 | play / select · SEND | NEED EVACUATION |
| top-right | L2 × R1 | channel up | SAFE |
| bottom-left | L1 × R2 | inbox prev · CANCEL | NEED MEDICAL |
| bottom-right | L2 × R2 | channel down | WATER GROUND FLOOR |

**Currently the bottom-right key doubles as the PTT** (`FM_TEST_PTT_ON_R2 = 1`),
so channel-down and the WATER alarm are unavailable until the side switch is
fitted to GPIO 47.

---

## 3. Microphone — INMP441

| Module pin | Connect to |
|---|---|
| WS | GPIO 41 |
| SCK | **GPIO 42** |
| SD | GPIO 40 |
| L/R | **GND** — must not float, or the output slot is undefined |
| VDD | 3V3 |
| GND | GND |

`SCK` on GPIO 42 is the correction that made the microphone work at all. An
earlier pin list said GPIO 45; the board was always on 42, and the mismatch meant
the mic was never clocked and returned 80,000 consecutive zero samples.

---

## 4. Amplifier — MAX98357A

| Module pin | Connect to |
|---|---|
| LRC | GPIO 19 |
| BCLK | GPIO 20 |
| DIN | GPIO 48 |
| SD | **GPIO 26**, **plus 1 kΩ from SD to GND** |
| GAIN | **100 kΩ to GND** → 15 dB |
| Vin | 3V3 |
| GND | GND |
| Speaker + / − | screw terminal (polarity irrelevant, bridged output) |

**The 1 kΩ pulldown on `SD` is required.** GPIO 26 comes out of reset with an
internal pull-up, which divides against the amp's internal 100 kΩ pulldown to
about 2.3 V — above the 1.4 V threshold — so without it the amplifier is fully
enabled from power-up until firmware runs. Do not substitute a larger value:
4.7 kΩ lands near 0.30 V, which is still an *enabled* band.

**`GAIN` floating selects the amplifier's LOWEST setting (9 dB)**, not a neutral
default. 100 kΩ to GND selects 15 dB.

### Decoupling, at the amplifier

| Part | Between |
|---|---|
| 100 nF ceramic (marked 104) | Vin and GND, **as close to the pins as possible** |
| 1000 µF electrolytic | Vin and GND, may sit a few cm away |

The ceramic must be close because it supplies the fast switching edges. The bulk
capacitor handles slow current demand and tolerates distance. **Observe polarity
on the electrolytic — stripe to GND.**

---

## 5. Buzzer

| Connection |
|---|
| GPIO 2 → 1 kΩ → 2N2222 **base** |
| 2N2222 **emitter** → GND |
| 2N2222 **collector** → buzzer negative |
| buzzer positive → **3V3** |

**Not the `5V` pin** — that is USB VBUS and reads 0 V on battery, which would
leave the annunciator silent exactly when mains and USB are gone.

2N2222 pinout varies between manufacturers in TO-92. Verify with a multimeter in
diode mode before soldering.

Add a 1N4007 across the buzzer, cathode to the 3V3 side, **only if** using a
magnetic transducer. A piezo is capacitive and does not need it.

---

## 6. Power

| Connection |
|---|
| Battery → Heltec **JST-GH 1.25 mm** header (not JST-PH 2.0) |
| Single cell only — **3.7 V nominal**. A 2S pack at 8.4 V destroys the board |
| Optional SPDT switch inline on the battery positive |

If paralleling cells for capacity, match them within 0.05 V before connecting.

---

## 7. Antenna

| Connection |
|---|
| Heltec U.FL socket → U.FL-to-SMA-female pigtail → SMA-**male** antenna, 863–928 MHz |

**Not RP-SMA** — it mates mechanically but the centre pin is inverted.

Never transmit without an antenna fitted; an unterminated output can damage the
SX1262's power amplifier.

---

## 8. Not yet fitted

| Item | Where | Consequence of absence |
|---|---|---|
| Side PTT switch | GPIO 47 to GND | bottom-right key is borrowed; no channel-down, no WATER alarm |

---

## 9. Deviations from the perfboard docs

`docs/perfboard-routing.md` and `docs/perfboard-18x24-build.md` were written
before the board was assembled and contain one net that no longer matches:

| Net | Those docs say | **As built** |
|---|---|---|
| Amp shutdown (`AMP_SD`) | GPIO 39 (J3-10) | **GPIO 26** |

GPIO 26 is `SPICS1` on the ESP32-S3 — the chip select for a second SPI device,
which on this part would be PSRAM. The S3FN8 has none, so nothing drives it and
it is free. Heltec break it out at J2-15. GPIO 39 would also have worked; the
board simply went to 26, and the firmware follows the board.

Those documents remain useful for their physical routing and grouping advice.
Take pin numbers from here or from `include/floodmesh_pins.h`.
