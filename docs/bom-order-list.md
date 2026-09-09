# FloodMesh — component order list

Written 9 Sept 2026, after board 1 was built and brought up.

Quantities assume **two complete units**, because a mesh cannot be tested with
one and the range test needs a real receiver at the far end, not a bare board.
Board 1 is already built; board 2 is currently a bare Heltec running the
transmitter-only firmware.

---

## 1. Already on board 1 — confirm you have a second set

| Item | Qty per unit | Notes |
|---|---|---|
| Heltec WiFi LoRa 32 V3.2 | 1 | you have 2 |
| 2×2 tactile keypad module | 1 | the **matrix** type, 4 pads, no ground |
| INMP441 I²S microphone module | 1 | |
| MAX98357A I²S amplifier module | 1 | |
| Speaker, 4 Ω or 8 Ω, 1–5 W | 1 | 8 Ω draws half the current of 4 Ω on the 3V3 rail |
| 1000 µF 16 V electrolytic | 1 | amplifier bulk decoupling |
| 100 nF ceramic (marked **104**) | 1 | amplifier HF decoupling, close to Vin |
| 1 kΩ resistor | 1 | amp `SD` pulldown to GND — **required**, the pin idles high at reset |
| 2N2222 NPN transistor | 1 | buzzer driver |
| 1 kΩ resistor | 1 | 2N2222 base resistor |

---

## 2. Still needed — these will stop you

| Item | Qty per unit | Why |
|---|---|---|
| **Antenna, 863–928 MHz, SMA male** | 1 | **Check you have two.** Transmitting into an unterminated output can damage the SX1262's PA |
| **IPEX (U.FL) to SMA-female pigtail** | 1 | The Heltec has a U.FL socket, not SMA. Easy to forget entirely |
| **Passive piezo buzzer** | 1 | Must be **passive** (KY-006), not active (KY-012). Passive gives pitch control; active gives one fixed note |
| **100 kΩ resistor** | 1 | `GAIN` to GND on the MAX98357A → 15 dB instead of the 9 dB you get with it floating |
| **Side tactile push button** | 1 | The PTT on GPIO 47. Until this is fitted the bottom-right key is borrowed for it, and the WATER alarm is unreachable |
| **JST-GH 1.25 mm 2-pin pigtail** | 1 | See the gotcha section — this is the one that catches people |
| SPDT toggle switch | 1 | Inline battery power switch |

---

## 3. Power

| Item | Qty per unit | Notes |
|---|---|---|
| 18650 cell, 3000 mAh | 2 | Or a single LiPo. **Match voltages within 0.05 V before paralleling** |
| 18650 holder / sled | 1 | |
| JST-GH 1.25 mm 2-pin pigtail | 1 | listed above; repeated because it is the usual surprise |

---

## 4. Consumables — buy generously, they cost nothing

| Item | Notes |
|---|---|
| Perfboard, 18×24 holes or larger | one per unit, plus spares |
| Silicone stranded hookup wire, 26–28 AWG | silicone flexes and survives repeated handling; PVC cracks |
| Male and female header pins / sockets | socket the modules rather than soldering them down — see below |
| Solder, flux, wick, isopropyl | |
| Heat-shrink, assorted | |

---

## 5. Enclosure — only when the electronics are settled

| Item | Notes |
|---|---|
| IP65/IP67 clear-lid polycarbonate box, ~100×68×40 mm | clear lid means the OLED reads without milling a window |
| Cable gland or SMA bulkhead | for the antenna |
| Silicone membrane / rubber button caps | for the keypad through the lid |

Do not buy this yet. The board layout will change at least once more.

---

## 6. The things that actually catch people

**The battery connector is JST-GH 1.25 mm, not JST-PH 2.0 mm.** They look alike
in photos and are completely incompatible. Almost every "LiPo battery with JST"
sold for hobby use is PH 2.0. Buy the GH 1.25 pigtail separately, or you will
have a charged cell and no way to connect it.

**The antenna connector is U.FL / IPEX, not SMA.** The board has no SMA socket.
You need a U.FL-to-SMA-female pigtail, then an SMA-**male** antenna. Not RP-SMA
— it looks identical but the centre pin is inverted and it will not mate.

**Buy passive, not active, buzzers.** Vendors call both "buzzer". The word you
want in the description is *passive* or *external drive*. An active buzzer has
its oscillator built in and can only make one note, which is not enough to give
eight different events distinguishable sounds.

**2N2222 pinout varies between manufacturers** in TO-92. Some are E-B-C, some
C-B-E. Check yours with a multimeter in diode mode before soldering: the base
is the pin that reads ~0.65 V forward to both others with the red probe on it.

**Order 3–5 of every small part.** Resistors, transistors, buzzers, capacitors
are pennies each. A single dead component with no spare costs a day of waiting
for delivery — and today's session lost hours to a single wrong pin.

**Socket the modules, don't solder them flat.** Female headers let you swap a
suspect microphone or amplifier in seconds instead of desoldering a 6-pin
module. Given how long it took to prove the microphone was wired to the wrong
clock pin, being able to swap parts quickly is worth the extra height.

---

## 7. Optional, depending on decisions still open

| Item | Needed if |
|---|---|
| 1N4007 diode | you use a **magnetic** transducer instead of a piezo — it is a coil and needs flyback protection. A piezo is capacitive and does not |
| MT3608 boost module | the buzzer proves too quiet at 3.3 V. Wire its `EN` from GPIO 2 alongside the transistor base so it draws nothing when idle and adds no switching noise while the radio listens |
| PC motherboard beeper | salvage source for a passive magnetic transducer if you want to try one — any computer repair shop has them |
| 100 nF ceramics, extra | decoupling for the mic module too, if you see noise |
