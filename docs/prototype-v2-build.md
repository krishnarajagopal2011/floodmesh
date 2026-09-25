# Prototype v2 perfboard build — Heltec V3, full feature set

Build sheet for the 15 perfboard prototypes (plus 1 bare logger unit) used to
test mesh, relays, sleep, responder registration, text input and homing
before the custom PCB is changed. See `docs/architecture.md` for why each part
is here.

**Firmware:** build env `proto_v2` (normal use) and `proto_v2_selftest`
(checking a freshly soldered unit). See §11.

Build **one reference unit first**, verify every item in §9, then copy it
exactly.

---

## 1. What changes from the as-built board (`docs/wiring-as-built.md`)

| Change | Why |
|---|---|
| 2×2 keypad on GPIO 4–7 **removed** | Replaced by a 12-key pad |
| **3×4 keypad via MCP23017** on a second I²C bus (GPIO 5/6), interrupt on GPIO 4 | 12 keys on 3 ESP32 pins; any key wakes from deep sleep |
| **SOS side button on GPIO 7** | Must be an RTC-capable pin to wake from deep sleep. GPIO 47 (old PTT) is not. |
| **External-power sense on GPIO 3** | Automatic relay mode when on solar/power bank |
| **INMP441 VDD moved from 3V3 to Vext** | The mic draws ~1.4 mA whenever powered. On 3V3 it stays on in deep sleep and swamps every sleep-current measurement. |
| Side PTT on GPIO 47 **not fitted** | Record by holding a keypad key instead. GPIO 47 stays spare. |
| **Current-measurement jumper** in the battery lead | Measure sleep/listen current on any unit without desoldering |

Unchanged: LoRa, OLED, battery sense, mic data pins, amplifier, buzzer.

---

## 2. Pin summary

| GPIO | Net | Notes |
|---|---|---|
| 0 | PRG button (onboard) | Spare input; don't hold at reset |
| 1 | Battery sense ADC (onboard) | |
| 2 | Buzzer | via 1 kΩ to 2N2222 base |
| **3** | **External-power sense** | 100 k / 100 k divider from 5V. Strapping pin (JTAG select), inert unless the eFuse is burned; needs an `FM_GPIO3_REVIEWED` note in the pin map |
| **4** | **MCP23017 INTA** | Open-drain, 10 kΩ pull-up to 3V3. RTC pin, wakes from deep sleep |
| **5** | **I²C bus 2 SDA** | 4.7 kΩ pull-up to 3V3 |
| **6** | **I²C bus 2 SCL** | 4.7 kΩ pull-up to 3V3 |
| **7** | **SOS side button** | To GND when pressed, 10 kΩ pull-up to 3V3. RTC pin |
| 8–14 | LoRa (onboard) | DIO1 = GPIO 14, RTC-capable |
| 17, 18, 21 | OLED (onboard) | I²C bus 1 |
| 19, 20, 48 | Amp LRC / BCLK / DIN | |
| 26 | Amp SD | **plus 1 kΩ to GND** |
| 35, 36, 37 | LED, Vext control, ADC control (onboard) | |
| 40, 41, 42 | Mic SD / WS / SCK | |
| 47 | Spare | Old PTT pad, not fitted |

---

## 3. Keypad: MCP23017 (DIP-28) + 3×4 membrane keypad

### 3.1 MCP23017 connections

| MCP23017 pin | Name | Connect to |
|---|---|---|
| 9 | VDD | **3V3** (always on, not Vext; it must stay powered to wake the unit) + **100 nF to GND at the pin** |
| 10 | VSS | GND |
| 12 | SCL | GPIO 6 (4.7 kΩ pull-up to 3V3) |
| 13 | SDA | GPIO 5 (4.7 kΩ pull-up to 3V3) |
| 15, 16, 17 | A0, A1, A2 | GND → I²C address **0x20** |
| 18 | RESET | **3V3** (active low; must not float) |
| 20 | INTA | GPIO 4, **10 kΩ pull-up to 3V3** (firmware sets open-drain) |
| 19 | INTB | Not connected |
| 11, 14 | NC | Not connected |
| 21 | GPA0 | Keypad column 1 |
| 22 | GPA1 | Keypad column 2 |
| 23 | GPA2 | Keypad column 3 |
| 24 | GPA3 | Keypad row 1 |
| 25 | GPA4 | Keypad row 2 |
| 26 | GPA5 | Keypad row 3 |
| 27 | GPA6 | Keypad row 4 |
| 28 | GPA7 | **Not used.** Recent datasheet revisions make GPA7/GPB7 output-only, so keep inputs off them |
| 1–8 | GPB0–GPB7 | Spare |

Columns are inputs (internal pull-ups, interrupt on change). Rows are outputs,
held **low** while idle and asleep, so any key press pulls a column low and
fires INTA.

### 3.1b Using an MCP23017 breakout module instead of the bare chip
Modules label the I/O pins **A0–A7 / B0–B7** (= GPA0–7 / GPB0–7, not the
address pins) and handle RESET and the address on the module itself.

| Module pin | Connect to |
|---|---|
| VCC | Heltec 3V3 (never 5V) |
| GND | Heltec GND |
| SDA | GPIO 5 (add 4.7 kΩ to 3V3 only if the module has no pull-ups) |
| SCL | GPIO 6 (same) |
| INTA | GPIO 4 + 10 kΩ to 3V3 (modules rarely pull INTA up) |
| A0 / A1 / A2 | keypad COL1 / COL2 / COL3 |
| A3 / A4 / A5 / A6 | keypad ROW1 / ROW2 / ROW3 / ROW4 |
| A7, B0–B7, INTB | not connected |

Default address 0x20. If the module's address pads are bridged, build with
`-D FM_KP_I2C_ADDR=0x21` (…`0x27`) in `platformio.ini` under `[env:proto_v2]`.

### 3.2 Keypad pinout: measure it, don't trust the listing
3×4 membrane keypads have 7 pins, but the order varies between sellers.
Usually it's R1 R2 R3 R4 C1 C2 C3, left to right, but **check with a
multimeter in continuity mode**: hold a key and find which two pins connect.
Write the result on the reference unit's sheet. Every keypad in the batch
should match, but check the first three.

---

## 4. SOS side button

| Connection |
|---|
| One leg → GPIO 7 |
| Other leg → GND |
| 10 kΩ from GPIO 7 to 3V3 |

Mount it where it can't be pressed by accident (recessed, or under a TPU cap).
Firmware requires a 3 s hold anyway.

---

## 5. External-power sense

| Connection |
|---|
| Heltec **5V** pin → 100 kΩ → **GPIO 3** |
| GPIO 3 → 100 kΩ → GND |

The 5V pin is USB VBUS and reads 0 V on battery, so GPIO 3 reads ~2.5 V on
USB/solar/power bank and ~0 V on battery. The divider only draws current when
USB is connected.

---

## 6. Microphone: INMP441 (changed rail)

| Module pin | Connect to |
|---|---|
| WS | GPIO 41 |
| SCK | GPIO 42 |
| SD | GPIO 40 |
| L/R | **GND** (must not float) |
| **VDD** | **Vext** (the Heltec header pin labelled `Ve`), **not 3V3** |
| GND | GND |

Vext is switched by GPIO 36 (active LOW) and also powers the OLED. The firmware
already turns it on at boot. The mic now powers down with it in sleep.

---

## 7. Amplifier and buzzer (unchanged)

Wire exactly as `docs/wiring-as-built.md` §4 and §5:
- **MAX98357A:** LRC 19, BCLK 20, DIN 48, **SD 26 with 1 kΩ to GND**, GAIN 100 kΩ
  to GND, Vin **3V3** (not Vext: speaker current peaks are too large for it),
  100 nF close + 1000 µF bulk decoupling.
- **Buzzer:** GPIO 2 → 1 kΩ → 2N2222 base; emitter GND; collector to buzzer −;
  buzzer + to **3V3**. Add a 1N4007 across a **magnetic** buzzer (cathode to
  3V3).

---

## 8. Power

```
LiPo(+) ─ switch ─ [jumper J_MEAS] ─ Heltec JST-GH (+)
LiPo(−) ─────────────────────────── Heltec JST-GH (−)
```

- The connector is **JST-GH 1.25 mm, not JST-PH 2.0**. **Check polarity** before
  plugging in; vendors wire it both ways.
- **J_MEAS:** a 2-pin header with a jumper. To measure current, pull the jumper
  and clip a multimeter (µA/mA range) across the two pins.
- Use the same antenna model on every unit. Never power up without the antenna.

---

## 9. Reference unit checklist (do all of these before copying)

| # | Check | Pass |
|---|---|---|
| 1 | Boots, OLED shows the call sign | |
| 2 | Battery voltage on the OLED is plausible | |
| 3 | I²C scan on bus 2 finds **0x20** | |
| 4 | All 12 keys detected, correct positions | |
| 5 | A key press wakes the unit from deep sleep | |
| 6 | SOS button wakes the unit from deep sleep | |
| 7 | GPIO 3 reads ~2.5 V on USB, ~0 V on battery | |
| 8 | Buzzer audible (note dB at 1 m with a phone app) | |
| 9 | Mic record → playback works (Vext rail) | |
| 10 | Amp silent at power-up (no pop or hiss before firmware runs) | |
| 11 | LoRa: two units exchange an alarm | |
| 12 | **Deep-sleep current** via J_MEAS, OLED off: ______ µA | |
| 13 | Light sleep + radio listening current: ______ mA | |

Items 3–7 and 12 are covered by the self-test firmware (§11.2).

---

## 10. Parts per unit

| Part | Qty |
|---|---|
| Heltec WiFi LoRa 32 V3 + 868 MHz antenna | 1 |
| MCP23017 (DIP-28) + 28-pin socket (recommended) | 1 |
| 3×4 membrane keypad | 1 |
| Tactile/panel push button (SOS) | 1 |
| INMP441 module | 1 |
| MAX98357A module + small speaker | 1 |
| Buzzer + 2N2222 (+ 1N4007 if magnetic) | 1 |
| 1S LiPo with protection + JST-GH lead | 1 |
| Slide switch | 1 |
| 2-pin header + jumper (J_MEAS) | 1 |
| Resistors: 100 kΩ ×3 (divider ×2, amp GAIN), 10 kΩ ×2 (INT, SOS), 4.7 kΩ ×2 (I²C), 1 kΩ ×2 (amp SD, buzzer base) | 9 |
| Capacitors: 100 nF ×2 (MCP23017, amp), 1000 µF ×1 (amp) | 3 |

---

## 11. Firmware for this build

### 11.1 Flashing
```bash
pio run -e proto_v2_selftest -t upload --upload-port COMx   # check a new unit
pio run -e proto_v2          -t upload --upload-port COMx   # normal firmware
```
GitHub Actions also builds both (see `.github/workflows/build.yml`); the
`firmware-*` artifacts on each run contain `firmware.bin`.

### 11.2 Self-test mode (`proto_v2_selftest`)
One live screen: keys held, SOS state, battery and USB voltage, keypad chip
found or not, role, last result, last alarm received.

| Key | Test |
|---|---|
| 1 | Buzzer, 300 ms |
| 2 | Speaker: 1 kHz tone for 1 s |
| 3 | Mic: record 2 s, show peak/RMS, play it back |
| 4 | LoRa: send a SAFE alarm (another unit shows `RX SAFE from …`) |
| 5 | Crypto: generate key, sign, verify, tamper check (`PASS`) |
| 7 | Deep sleep with OLED, mic rail and radio off. **Measure current at J_MEAS now.** Any key or SOS wakes it (it reboots) |

### 11.3 Normal mode (`proto_v2`)
| Keys | Action |
|---|---|
| 2 / 8 | Channel up / down |
| 4 | Inbox previous |
| 5 | Play / stop the selected clip |
| hold `*` | Record voice (up to 10 s); release opens the 3 s send window: hold 5 to send now, tap 4 to cancel |
| hold `*` + 2 / 8 / 4 / 5 | Alarm: SAFE / WATER / MEDICAL / EVACUATE |
| **hold SOS 3 s** | **SOS alarm** (countdown on screen; release to cancel) |

At power-on:
| Hold | Effect |
|---|---|
| `#` | **Bluetooth provisioning mode** for the FloodMesh Admin app (shows a PIN) |
| SOS + `0` for 10 s | Factory reset: admin key, role, responder key, call-sign override |

The top-right of the standby screen shows the role (`RLY`/`RSP`), a responder
expiry warning (`!2d`), `USB` when external power is present, and battery %.

Serial console (115200): `help`, `info`, `prov` (reboot into provisioning),
`alarm <0-4>` (send SAFE/MEDICAL/WATER/EVACUATE/SOS from a laptop, useful
for stress tests).

### 11.4 What the roles do in this firmware
Registration, expiry, key handling and the on-screen role are complete.
**The relay and responder mesh behaviours are not implemented yet** (beacons,
batched ACKs, signed responder messages, mesh-mode volunteering). A relay or
responder unit currently behaves like a civilian unit on air. Those are the
next firmware steps, tested with these same units.
