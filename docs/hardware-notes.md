# FloodMesh — hardware & spec review notes

## 0. Current wiring (2×2 keypad revision)

```
        L1 [top-left]  GPIO 7        R1 [top-right]  GPIO 5
        L2 [bot-left]  GPIO 6        R2 [bot-right]  GPIO 4
```

| Net | GPIO | Note |
|---|---|---|
| Keypad L1 / L2 / R1 / R2 | 7 / 6 / 5 / 4 | INPUT_PULLUP, other leg to GND |
| Side PTT | 47 | J2-13. `PTT_HARDWARE_INSTALLED 0` while unsoldered |
| Buzzer | 2 | 2N2222 base via 1 kΩ; collector rail **3V3** |
| Mic WS / SCK / SD | 41 / 42 / 40 | INMP441, own bus (I²S_NUM_0). **L/R must be tied to GND** — floating leaves the output slot undefined |
| Amp LRC / BCLK / DIN | 19 / 20 / 48 | MAX98357A, own bus (I²S_NUM_1) |
| Amp sleep gate | 39 | **not 26** — see below |

**GPIO 26 is not usable.** On the ESP32-S3 it is `SPICS1`, part of the SPI
flash bus (26–32). It is not on Heltec's usable list, is not broken out on the
V3 header, and is rejected by a `static_assert` in `floodmesh_pins.h` — a pin in
that range is not a wiring preference, it is a board that cannot fetch code.
GPIO 39 (J3-10) has no boot-time role and is verified free.

**The microphone and amplifier keep separate buses.** Each gets its own clock
pair and its own I²S peripheral, so neither can disturb the other's clocking.
The mic runs at 16 kHz and decimates ×2 in software — at 8 kHz its bit clock
would sit at 512 kHz, the very bottom of the INMP441's usable range.

**Verified working on hardware (2026-09-09):** OLED, 2×2 matrix keypad, battery
sense, buzzer, microphone capture, Codec2 encode/decode, LittleFS ring buffer,
speaker playback, LoRa two-node link, PSK-authenticated alarms, 8-fragment voice
reassembly. Measured airtime 2599 ms per 10-second note against 2556 ms
predicted — within 2%.

### The bit-clock incident, and the lesson

The microphone produced 80,000 consecutive zero samples. The written pin list
said `SCK → GPIO 45`; the board actually had it on **GPIO 42**. An unclocked
INMP441 never drives its data line, so `SD` sat tri-stated and every sample read
as exactly zero.

That one symptom is indistinguishable between four different faults: a dead
microphone, a broken data wire, the wrong I²S slot, or the wrong clock pin. It
took a raw 32-bit dump (to rule out a shift error), a slot swap (to rule out the
channel), and finally a continuity check on the board itself to corner it.

Note also that the GPIO float test was **inconclusive by design** — an idle
INMP441 tri-states `SD`, so "floating" is what a correctly wired but unclocked
mic looks like too. It ruled things in, never out.

**Measure the board. Do not trust the list.**

**With no PTT fitted the device cannot send any alarm.** Every Layer 1 alarm is
a PTT chord, so `PTT_HARDWARE_INSTALLED 0` leaves a receive-only pager that
relays and plays back but never originates. The firmware says so at boot.


Findings from the Milestone 1 review of the netlist and the milestone plan.
Ordered by how much rework they cause if discovered late.

---

## 1. BW = 250 kHz likely breaches the Indian 865–867 MHz delicensing terms

The delicensed 865–867 MHz allocation is granted with a **maximum carrier
bandwidth of 200 kHz** (alongside the 1 W / 4 W ERP limit). LoRaWAN IN865 uses
125 kHz channels for exactly this reason.

**Action:** use `BW = 125 kHz, SF7, CR 4/5` for Milestone 4 unless someone can
produce the notification text permitting 250 kHz.

Airtime cost of the change, for a ~200-byte fragment:

| Setting | Per fragment | Per 10 s voice note (8 fragments) |
|---|---|---|
| SF7 / BW250 / CR4-5 | ~160 ms | ~1.3 s |
| SF7 / BW125 / CR4-5 | ~320 ms | ~2.6 s |

At 3 hops a single voice note consumes roughly **8 s of channel occupancy**.
That is the number that governs the whole Layer 1 / Layer 2 priority design —
Layer 1 alarms must be able to preempt an in-flight voice burst, not merely
"win" at queue time.

---

## 2. The 5 V buzzer is dead on battery power — RESOLVED by moving to 3V3

**Decision:** the buzzer's positive lead goes to the regulated **3V3 bus**, not
the board's `5V` pin. The amplifier's `Vin` likewise. Measure the actual SPL at
3.3 V before accepting it — this is the one part that has to work when
everything else has failed.

If 3.3 V proves too quiet, in order of preference:

1. **A 3 V magnetic sounder** instead of a piezo. Magnetic types are
   current-driven and hold up far better below their rated voltage. No extra
   parts, no extra pin.
2. **An MT3608 boost**, wired so it costs nothing when idle — drive its `EN`
   pin from the same GPIO 2 that drives the transistor base:

   ```
   GPIO 2 ──┬── 1kΩ ── 2N2222 base
            └────────── MT3608 EN
   MT3608 OUT ── buzzer ── 2N2222 collector ── (emitter to GND)
   ```

   That gives no idle current, and no switching noise while the radio is
   listening, because the converter only runs during an alert. The transistor
   is still required: a *disabled* boost converter still passes roughly battery
   voltage through its inductor and catch diode, so without it the buzzer would
   murmur continuously. Note the converter needs a few ms to come up, so the
   12 ms UI clicks would sound weak — lengthen them to ~25 ms if this route is
   taken.

   A switching converter beside an SX1262 is a real receiver-desense risk on
   hand-wired perfboard, which is why it is second choice rather than first.

The original reasoning, for the record:

The Heltec V3 `5V` pin is USB VBUS passed through a diode. There is **no boost
converter** on the board, and the design deliberately removed the MT3608. On
18650 power the buzzer rail sits at 0 V — the alarm annunciator, the one part
that must work during a flood, is silent exactly when mains and USB are gone.

**Options:** (a) source a 3.3 V-rated active buzzer (most "5 V" modules still
sound at 3.3 V, at reduced SPL — measure it), (b) add a small boost dedicated to
the buzzer, or (c) drive a passive piezo differentially from two GPIOs via LEDC
to get ~6.6 Vpp from the 3.3 V rail.

Also note: a **1N4007 flyback diode is for inductive loads**. An active piezo
buzzer is capacitive; the diode is harmless but does nothing. Keep it only if
the chosen annunciator is a magnetic (coil) sounder.

---

## 3. `MIC SD` was on GPIO 45, a strapping pin — RESOLVED, now GPIO 40

GPIO 45 is `VDD_SPI` strapping on the ESP32-S3 — its level at reset selects the
flash voltage rail. If the INMP441 drove it HIGH while the board came out of
reset, the board could fail to boot in a way that looks like a dead module.
On the V3 header this is J3-6.

**Resolved:** `MIC SD` is now **GPIO 40** (J3-9). Nothing was soldered yet, so
this cost nothing.

Note that **GPIO 38 is not a valid alternative** despite being physically free:
Heltec's own GPIO guidance groups it with the SPI-Flash/SubSPI pins (33–38).
GPIO 46 is also out — it is the boot-mode strapping pin (J3-5).

The usable pins on this board, per Heltec's pin-diagram guidance:

| Pins | Status |
|---|---|
| 1, 2, 4, 5, 6, 7, 19, 20, 47, 48 | Recommended, no onboard function |
| 39, 40, 41, 42 | Pin-based JTAG pads (J3-10, J3-9, and the default I²C pair). Free by default, because the S3 ships using USB-Serial-JTAG on 19/20 instead |
| 45, 46 | Strapping — avoid |
| 38 | Reserved (SPI Flash / SubSPI group) |

The consequence of using 39–42 **and** 19/20: this board has no hardware debug
port left, over USB or over pins. Debugging is serial-console only. That is an
acceptable trade for a device with this pin count, but it should be a decision
rather than a surprise the first time someone reaches for a JTAG probe.

---

## 4. GPIO 19 / 20 are the native USB D-/D+ pins

Usable as I2S on this board, because the USB socket is wired to the onboard
UART bridge rather than to the S3's native USB. The costs are: no USB-CDC, no
USB-JTAG debugging, and the USB Serial/JTAG peripheral driving those pads
briefly at reset. Acceptable — but keep the serial console on UART0
(`ARDUINO_USB_CDC_ON_BOOT=0`, already set in `platformio.ini`).

---

## 5. No GPIO allocated for MAX98357A `SD` (shutdown)

The architecture calls for the amplifier to be held in hard shutdown and woken
only for playback, but the netlist assigns no pin to it. Without it the amp
idles at ~2 mA forever.

**Action:** allocate one spare GPIO (e.g. 38/39/40 — whichever the mic does not
take) to `SD`. `SD` low = shutdown; `SD` above ~1.4 V = enabled. Note the same
pin also selects the amp's channel mode via its resistor divider, so drive it
push-pull and keep the module's stock 100 kΩ pulldown in mind.

**Resolved:** `SD` is **GPIO 39** (J3-10). GPIO 47 and 48 are confirmed present
on the header (J2-13 and J2-14) and carry no onboard function, so PTT and amp
DIN stay where they are.

The `SD` wire is optional. Without it the module's own pull-up holds the
amplifier awake and it costs ~2 mA continuously; everything else works.

---

## 6. Codec2 needs 8 kHz PCM; the INMP441 does not like 8 kHz

Codec2 1200 bps consumes 8 kHz / 16-bit mono. At `fs = 8 kHz` with 64 SCK per
frame the INMP441 bit clock lands at 512 kHz, right at the bottom of its
specified range.

**Action:** run I2S at **16 kHz** and decimate by 2 with a half-band FIR into
the codec. Also remember the INMP441 delivers 24-bit samples left-justified in
32-bit slots — take the top bits, do not truncate the word.

---

## 7. Fragmentation should be Codec2-frame aligned, and the header needs a message ID

The payload maths is exact: Codec2 1200 = 48 bits per 40 ms frame → 250 frames
in 10 s → **1500 bytes**. But 1500 / 8 = 187.5, so a byte-count split cuts a
6-byte codec frame in half at every boundary.

**Action:** split on frame boundaries — `7 × 32 frames (192 B) + 1 × 26 frames
(156 B)` = 250 frames. A lost fragment then costs only its own frames and the
decoder can conceal the gap instead of desynchronising.

**Implemented header — 10 bytes, one more than the 9-byte proposal:**

```
[ Ver+Type(1B) | HopLimit(1B) | CallSign(5B) | ChannelID(1B) | MsgID(1B) | FragIdx:4|TotalFrags:4 ]
```

Two notes on how this differs from the 9-byte version:

- **The hop limit is the added byte, and it is not optional.** Managed flooding
  with no TTL does not terminate — every node rebroadcasts every frame forever
  the moment the topology contains a loop. One byte buys termination.
- **Version and type share byte 0** (high nibble `0xF` = FloodMesh magic, low
  nibble = frame type), so the type costs nothing. A protocol revision changes
  the whole byte.

`MsgID` stays **one byte**, and the arithmetic supports it: the 2.5% duty cycle
caps a node near 35 voice notes an hour, so wrapping 256 ids takes about seven
hours, while the dedup ring forgets an entry after ten minutes. The wrap can
never catch the memory, so a second byte would buy nothing but airtime.

192 B payload + 10 B header = 202 B, comfortably inside the SX1262's 255-byte
limit. The Layer 1 alarm frame is 22 bytes.

---

## 8. Layer 1 authentication needs replay protection, not just a MAC

A 16-byte PSK with a truncated HMAC-SHA256 (or AES-CMAC via mbedtls, already in
the IDF) authenticates *origin*. In a store-and-forward mesh where every node
rebroadcasts, an attacker — or a buggy node — can replay a captured "MEDICAL
EMERGENCY" frame indefinitely.

**Action:** include a monotonic counter or timestamp inside the MAC'd region and
keep a small per-callsign seen-window at each node. This also gives the mesh its
duplicate-suppression for free.

---

## 9. Memory: encode in real time, do not buffer raw audio

The ESP32-S3 has 512 KB of on-chip SRAM in total, but that figure is misleading:
part of it is IRAM. The DRAM the heap and `.bss` actually draw from is
**320 KB** — PlatformIO's own board file says so
(`heltec_wifi_lora_32_V3.json:41`, `"maximum_ram_size": 327680`), and there is
no PSRAM. 10 s of raw 16 kHz / 16-bit audio is 320 KB: it does not merely fit
badly, it does not fit at all.

**Action:** run Codec2 encode per 40 ms frame during capture, so RAM holds only
a small PCM ring plus the growing 1500-byte encoded buffer. Milestone 2's 5-second
RAM loopback (80 KB at 8 kHz) is fine as a bring-up test, but is not the shape
the final recorder should take. Keep WiFi and BT out of the build to preserve heap.

---

## 10. Minor

- **ADC_Ctrl polarity** differs across V3 sub-revisions. `ADC_CTRL_ENABLE_LEVEL`
  is a build flag in `platformio.ini`; flip it to `HIGH` if VBAT reads ~0 V.
- **Paralleling 18650s:** charge both cells to the same voltage before wiring
  them in parallel, or the inrush between them can be hundreds of amps.
  Charging 6000 mAh through the onboard charger takes the better part of a day.
- **Runtime claim:** 40–55 h assumes duty-cycled operation. Continuous RX with
  the OLED lit is ~70–80 mA → ~75 h at 6000 mAh in the best case, less in
  practice. Light sleep between RX windows is what makes the "days" claim true.
