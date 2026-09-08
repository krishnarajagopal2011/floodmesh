# FloodMesh — 18 × 24-hole hand-soldering manual

For the user's 50 × 70 mm perfboard: **Heltec V3, INMP441 and MAX98357A all mount on the same board**. Speaker, buttons, buzzer and battery mount in the enclosure. This supersedes the remote-module placement in `perfboard-routing.md`. Firmware assignments come from `include/floodmesh_pins.h`; no firmware changes are needed.

**Electrical assignments checked; mechanical fit provisional.** Actual breakout dimensions/pad order, perfboard edge margins and socket spacing are not yet measured. Dry-fit before soldering. The drawing is a wire-routing guide, not an etching mask or a 1:1 drilling template.

## Orientation and physical fit

Component side toward you; 50 mm across, 70 mm vertically; antenna end at top, USB at bottom. Mark columns **A–R**, left to right; rows **1–24**, top to bottom. At 2.54 mm pitch the outer hole centers span 43.18 × 58.42 mm. Actual edge margins vary.

Candidate 18-way socket positions: **J3 = E1:E18, J2 = N1:N18**. That assumes **22.86 mm header center spacing**. Fit sockets onto the actual unpowered Heltec as a jig and check the fit; do not bend pins to force it. Use female sockets compatible with the installed header pins. Pin 18 is at row 1; pin 1 is at row 18. **Carrier row = 19 − header pin number.** GPIO numbers are different from header pin numbers.

For an approximately centered hole grid, reserve these physical envelopes, measured from the perfboard's top-left corner:

| Module | Available placement envelope | Mounting |
|---|---|---|
| Heltec | Centered horizontally, approximately y = 0–51 mm | Removable sockets |
| Mic | x = 1–18, y = 52–69 mm; max 17 × 17 mm | Insulated supports and short solder tails |
| Amp | x = 30–49, y = 51–69 mm; max 19 × 18 mm, rotated if necessary | Insulated supports and short solder tails |

These are available-space estimates, **not assumed breakout dimensions**. Dry-fit with a USB cable inserted. Support the audio boards about 3–5 mm above the carrier with nylon spacers or insulating brackets; solder tails are not mechanical supports. Keep the mic's acoustic port exposed. If its bottom port faces the carrier, provide an unobstructed acoustic opening or mount it upright. Cut any opening before assembly with the microphone removed.

If a module exceeds its envelope or blocks USB, mount it **upright at its lower board edge**, using an insulating bracket and the same short tails. All three still mount on this perfboard. A guaranteed flat footprint needs the actual breakout dimensions and pad order. Keep speaker wiring away from the mic and antenna; attach the correct antenna before transmitting.

## Carrier component and landing-pad positions

JM/JA are **carrier solder landing pads**. They do not assume your breakout's pin order. Connect named module pads with short insulated tails; do not plug an unknown breakout straight into these positions.

| Reference | Holes in order | Connections in the same order |
|---|---|---|
| JB, button connector | A3, A4, A5, A6, A7, A8 | GND, UP, DOWN, LEFT, RIGHT, PTT |
| JM, mic landing pads | A22, B22, C22, D22, E22, F22 | GND, 3V3, SCK, WS, SD, L/R-ground |
| JA, amp landing pads | M22, N22, O22, P22, Q22, R22 | GND, 3V3, DIN, BCLK, LRC, shutdown |
| JZ, buzzer cable | Q18, R18 | Positive, switched negative |
| Ground bus | A19 through R19 | Continuous tinned copper wire |
| 3.3 V bus | A20 through R20 | Continuous tinned copper wire |
| R1, 1 kΩ | O10 to R10 | GPIO2 side at O10; base side at R10 |
| Q1 landing points | Q12, Q13, Q14 | Emitter, base, collector |
| R2, 10 kΩ | O13 to O16 | O13 wired to base; O16 wired to emitter |

Verify the actual transistor's E/B/C order; 2N2222-family TO-92 pinouts are not universal. Use insulated lead extensions if necessary to reach the assigned landing points. R1 = 1 kΩ is intended for a low-current 3.3 V active buzzer, about 20 mA or less; a larger sounder needs its driver sized accordingly.

## Signal wire list

Use insulated **28–30 AWG** wire. Each row is one continuous wire, stripped/soldered **only at the endpoints**. Intermediate coordinates are bend guides, not solder joints. Wires may cross pads, other wires and bare buses only with insulation intact. Shift overlapping bends slightly between holes instead of stacking wires directly on one another.

| Net | Heltec source | Start | Bend guides | End |
|---|---|---|---|---|
| UP | J3-15 / GPIO4 | E4 | Direct | A4 |
| DOWN | J3-16 / GPIO5 | E3 | B3 → B5 | A5 |
| LEFT | J3-17 / GPIO6 | E2 | C2 → C6 | A6 |
| RIGHT | J3-18 / GPIO7 | E1 | D1 → D7 | A7 |
| PTT | J2-13 / GPIO47 | N6 | L6 → L8 | A8 |
| Mic SCK | J3-7 / GPIO42 | E12 | C12 | C22 |
| Mic WS | J3-8 / GPIO41 | E11 | D11 | D22 |
| Mic SD (mic → ESP32) | J3-9 / GPIO40 | E10 | F10 → F21 → E21 | E22 |
| Amp DIN | J2-14 / GPIO48 | N5 | O5 | O22 |
| Amp BCLK | J2-17 / GPIO20 | N2 | P2 | P22 |
| Amp LRC | J2-18 / GPIO19 | N1 | R1 → R21 → Q21 | Q22 |
| Amp shutdown | J3-10 / GPIO39 | E9 | M9 → M21 → R21 | R22 |
| Buzzer control | J3-13 / GPIO2 | E6 | H6 → H10 | O10 / R1 input |
| Base feed | R1 output | R10 | Direct | Q13 / base |
| Collector | Q1 | Q14 | R14 | R18 / buzzer − |
| R2 base | Q1 base | Q13 | Direct | O13 / R2 upper end |
| R2 return | R2 lower end | O16 | Direct | Q12 / emitter |

Here **R1 in a bend path is column R, row 1**, whereas the resistor reference R1 means the 1 kΩ part at O10:R10. Keep amp wires insulated above the buzzer leads that they cross. Allow service slack without tall loops. No bare signal tracks or long solder beads.

## Power and ground wire list

Use **22–24 AWG** wire for buses, power branches and speaker leads. Use bare tinned wire only for the two straight buses; all branches crossing other nets stay insulated. Solder each bus to its row of pads. Ground and 3.3 V are separate adjacent rows.

| From | To | Purpose |
|---|---|---|
| E18 / J3-1 | E19 | Ground feed |
| N18 / J2-1 | N19 | Ground feed |
| E17 / J3-2 | E20 | 3.3 V feed, insulated across ground bus |
| A19 | A3 | Button common |
| A19 | A22 | Mic ground |
| F19 | F22 | Mic L/R ground |
| B20 | B22 | Mic power |
| M19 | M22 | Amp ground, separate branch from mic |
| N20 | N22 | Amp power |
| Q19 | Q12 | Q1 emitter ground |
| Q20 | Q18 | Buzzer positive, insulated across ground bus |

This retains the project's **3.3 V supply for all three peripheral loads**. Do not use Vext or the USB-dependent 5 V pin for this wiring. Start with one 8 Ω speaker and low volume. The shared rail needs bench testing at intended radio power and playback volume; a published current rating does not prove simultaneous peak-load stability. If it sags/resets, use a separately rated regulated amp supply with common ground, disconnecting JA-VIN from the carrier 3.3 V bus. Never parallel regulator outputs. [Heltec power specifications](https://resource.heltec.cn/download/WiFi_LoRa_32_V3/HTIT-WB32LA_V3.2.pdf).

## All microphone, amplifier and speaker pins

**Your actual breakout order**, in the orientation you described (keep that orientation while making the tails):

```text
Amp, left → right:
LRC    BCLK   DIN    GAIN    SD     GND    VIN
Q22    P22    O22    open    R22    M22    N22   ← carrier destinations

Mic top row, left → right:
SD     VDD    GND
E22    B22    A22                               ← carrier destinations

Mic bottom row, left → right:
L/R    WS     SCK
F22    D22    C22                               ← carrier destinations
```

The mic's two-row spacing and both breakout dimensions are still unmeasured. These are **wire destinations, not a direct-insertion footprint**. Route tails below the modules with insulation at crossings; label both ends before mounting. GAIN gets no external wire. Speaker output terminals are separate from the amp's seven-pin input row.

| Module pad | Goes to |
|---|---|
| INMP441 VDD / VCC | B22 / 3.3 V |
| INMP441 GND | A22 / ground |
| INMP441 SCK | C22 / GPIO42 |
| INMP441 WS | D22 / GPIO41 |
| INMP441 SD | E22 / GPIO40, microphone data output |
| INMP441 L/R | F22 / ground, left channel |
| INMP441 CHIPEN, if exposed | 3.3 V; typically already connected on six-pin breakouts |
| MAX98357A VIN / VDD | N22 / 3.3 V |
| MAX98357A GND | M22 / ground |
| MAX98357A DIN | O22 / GPIO48 |
| MAX98357A BCLK | P22 / GPIO20 |
| MAX98357A LRC / LRCLK | Q22 / GPIO19 |
| MAX98357A SD / SD_MODE | R22 / GPIO39; distinct from mic SD |
| MAX98357A GAIN | Leave externally unconnected for initial bring-up; inspect existing breakout straps |
| MAX98357A SPK+ / OUT+ | Speaker + only |
| MAX98357A SPK− / OUT− | Speaker − only |

Mic: fit **100 nF VDD-to-GND** and **100 kΩ SD-to-GND**, if not already on its breakout. Keep module tails preferably under 5 cm, with ground beside the audio bundle. Keep flux and debris out of the acoustic port. [INMP441 datasheet](https://invensense.tdk.com/wp-content/uploads/2015/02/INMP441.pdf).

Amp: provide **100 nF plus 10 µF** near VIN/GND if absent on its breakout, with an additional **47–100 µF bulk capacitor** at the wired supply entry. Electrolytic + to VIN, − to ground, rated at least 6.3 V. Twist the speaker wires together and route directly from amp to speaker. **Neither speaker terminal connects to GND.** One amp drives one mono load; adding parallel speakers changes the load and power requirement.

MAX98357A supports 3.3 V power and GPIO shutdown. HIGH on SD_MODE selects left-channel playback; firmware duplicates audio into both channels. An unstrapped GAIN pad gives 9 dB, but breakout straps can override this. Do not hard-wire SD to VIN while also connecting GPIO39. [MAX98357A datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX98357A-MAX98357B.pdf).

## Buttons, buzzer, battery and built-in connections

Each of the five **normally open switches** connects its JB signal to JB ground. Firmware provides pull-ups. For four-legged tactile switches, use a continuity meter to identify the two contact groups; legs in the same group are already connected internally. PTT is GPIO47; face buttons retain the firmware's navigation and PTT+alarm functions.

Buzzer circuit: GPIO2 → R1 1 kΩ → Q1 base; R2 10 kΩ base-to-emitter; emitter to ground; collector to buzzer −; buzzer + to 3.3 V. Use a **3.3 V active buzzer**. For a magnetic/coil sounder, add a diode across its terminals, **stripe/cathode to +, anode to collector**. A 1N400x suits simple on/off switching. A piezo active buzzer does not require this flyback diode.

Battery: suitable protected **1S Li-ion pack** to the Heltec battery connector, BAT+ to positive and BAT− to negative. Verify PCB/connector polarity, not just lead colors. Secure the holder off-board. Do not connect the battery to the 3V3/5V header or use a two-cell series pack. USB remains the programming/charging connection. A switch in a battery lead interrupts battery power only; USB can still power the board.

The **OLED and radio are already wired internally**. No carrier tracks for OLED SDA17/SCL18/RST21; SX1262 NSS8/SCK9/MOSI10/MISO11/RST12/BUSY13/DIO1-14; LED35; Vext control36; battery ADC1/control37; onboard PRG0/reset. Reserve those pins. GPIO19/20 carry audio here; retain stock UART-bridge USB wiring without native-USB rerouting modifications.

## All 36 Heltec header pins

“Leave” means no external carrier wire, not necessarily unused internally. Verified against the [official V3 pin map](https://resource.heltec.cn/download/WiFi_LoRa_32_V3/HTIT-WB32LA%28F%29_V3.png), saved as `docs/reference/heltec-v3-pinmap.png`.

| Row | Header pin (both sides) | Left J3 | Action | Right J2 | Action |
|---|---|---|---|---|---|
| 1 | 18 | E1 / 7 | RIGHT | N1 / 19 | Amp LRC |
| 2 | 17 | E2 / 6 | LEFT | N2 / 20 | Amp BCLK |
| 3 | 16 | E3 / 5 | DOWN | N3 / 21 | Leave |
| 4 | 15 | E4 / 4 | UP | N4 / 26 | Leave |
| 5 | 14 | E5 / 3 | Leave | N5 / 48 | Amp DIN |
| 6 | 13 | E6 / 2 | Buzzer | N6 / 47 | PTT |
| 7 | 12 | E7 / 1 | Leave | N7 / 33 | Leave |
| 8 | 11 | E8 / 38 | Leave | N8 / 34 | Leave |
| 9 | 10 | E9 / 39 | Amp SD | N9 / 35 | Leave |
| 10 | 9 | E10 / 40 | Mic SD | N10 / 36 | Leave |
| 11 | 8 | E11 / 41 | Mic WS | N11 / 0 | Leave |
| 12 | 7 | E12 / 42 | Mic SCK | N12 / RST | Leave |
| 13 | 6 | E13 / 45 | Leave | N13 / TX43 | Leave |
| 14 | 5 | E14 / 46 | Leave | N14 / RX44 | Leave |
| 15 | 4 | E15 / 37 | Leave | N15 / Vext | Leave |
| 16 | 3 | E16 / 3V3 | Leave | N16 / Vext | Leave |
| 17 | 2 | E17 / 3V3 | Supply | N17 / 5V | Leave |
| 18 | 1 | E18 / GND | Ground | N18 / GND | Ground |

## Soldering and bring-up sequence

1. Dry-fit sockets, audio boards, supports and USB plug. Mark A1 permanently. Remove modules before soldering. Confirm socket center spacing and board orientation.
2. Solder buses, power branches, buzzer driver and button connector. Fit low-profile buzzer parts outside the Heltec outline. Keep every crossing insulated.
3. Wire buttons, then mic, then amp. Inspect and meter each wire before another wire hides it. Do not solder bend points. Add audio module tails and local passives, then mount the modules securely.
4. With USB/battery disconnected and Heltec removed, inspect with magnification. Check end-to-end continuity, adjacent pads for bridges, and supply polarity at each breakout. There must be no near-zero-ohm supply short after capacitors charge; fitted electronics may show finite resistance.
5. Verify every switch is open at rest and shorts only its signal to ground when pressed. Confirm Q1 pinout, capacitor polarity, buzzer polarity and diode stripe if fitted.
6. Power the Heltec alone first with antenna attached. Power off before connecting peripherals. Add mic, then amp/speaker at low volume. Check capture, intelligible playback, buttons and buzzer.
7. Confirm GPIO39 LOW when idle and HIGH for playback. Test power cycling and intended maximum radio/audio/buzzer activity on USB and battery. Watch for resets, supply sag, noise and overheating. Physical fit and load stability remain bench checks; no assembled-hardware testing is claimed.

## Solder-side view

Flip **left-to-right, keeping the antenna end at the top**. Columns now appear **R … A** from your left to right; rows still increase downward. Hole names stay with the pads: E12 moves to the right side of the drawing but remains E12. The interactive view provides this mirror. Only endpoint dots identify solder joints; crossings are insulated.
