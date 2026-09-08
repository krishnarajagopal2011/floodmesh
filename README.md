# FloodMesh

An off-grid, store-and-forward emergency communicator for urban flooding.

When cellular base stations and mains power fail together — as they did in
Chennai in 2015 and during Cyclone Michaung — communities a couple of hundred
metres apart are cut off from each other entirely. FloodMesh is a decentralised
fallback: a handheld that hops authenticated distress statuses and short voice
clips around building obstructions, running for days on common lithium cells.

It is a **pager, not a walkie-talkie**. Because messages are asynchronous and
store-and-forward, they route around wet reinforced concrete instead of needing
line of sight, and the radio is idle most of the time instead of holding an open
channel.

---

## Two classes of traffic

| | Layer 1 — status alarms | Layer 2 — voice |
|---|---|---|
| Size | 22 bytes | 1500 bytes, 8 fragments |
| Airtime | ~70 ms | ~2.6 s |
| Authentication | 16-byte PSK, HMAC-SHA256 | none |
| Priority | never throttled, preempts voice mid-packet | best-effort, yields |
| Duty-cycle budget | exempt (deliberate life-safety choice) | refused when the budget is spent |

Four alarms: `SAFE`, `NEED MEDICAL`, `WATER GROUND FLOOR`, `NEED EVACUATION`.
Four channels: Medical, Rescue Boats, Volunteers, Supplies.

---

## Hardware

Heltec WiFi LoRa 32 V3.2 — ESP32-S3FN8, Semtech SX1262, 0.96" SSD1306 OLED.

| Net | GPIO | Notes |
|---|---|---|
| Keypad (2×2 **matrix**) | cols 7, 6 · rows 5, 4 | No ground pin; scanned row/column |
| Side PTT | 47 | Shift key: held, the face keys become alarms |
| Buzzer | 2 | 2N2222 via 1 kΩ, collector to **3V3** (not `5V` — that is USB VBUS, dead on battery) |
| INMP441 mic | WS 41 · SCK 42 · SD 40 | `L/R` **must** be tied to GND |
| MAX98357A amp | LRC 19 · BCLK 20 · DIN 48 | Own I²S peripheral |
| Amp shutdown | 39 | Needs a 1 kΩ pulldown to GND — the pin idles high at reset |
| Battery sense | ADC 1, gate 37 | Gate is **active HIGH** on V3.2, active LOW on V3.0/V3.1 |

Radio: 866.5 MHz, **BW 125 kHz**, SF7, CR 4/5, preamble 16.

The 125 kHz bandwidth is a **regulatory ceiling, not a tuning choice**. India's
G.S.R. 853(E) (2021) Table II caps this device class at 200 kHz and 500 mW
e.r.p., with a **2.5% duty cycle** — about 90 seconds of transmission per hour,
which the firmware tracks and enforces. The SX1262 offers no 200 kHz step, so
125 kHz is the widest legal setting.

See [`docs/hardware-notes.md`](docs/hardware-notes.md) for the full wiring
rationale, the pins that cannot be used and why, and the bring-up findings.

---

## Building

```bash
pio run                                  # build
pio run -t upload --upload-port COMx     # flash
pio run -t uploadfs --upload-port COMx   # format the LittleFS clip partition
```

Useful bench flags (command line, not in `platformio.ini`):

| Flag | Effect |
|---|---|
| `FM_DEBUG_TICK=1` | 2-second liveness tick with key states |
| `FM_SELF_MONITOR=1` | Save your own recordings to the inbox so you can play them back |
| `FM_MIC_RAW_DEBUG=1` | Dump raw 32-bit I²S words from the microphone |
| `FM_KEYPAD_DISCOVER=1` | Map an unknown keypad matrix |
| `FM_ROLE_TESTTX=1` | Transmit-only role for a bare second board (no peripherals needed) |

```bash
PLATFORMIO_BUILD_FLAGS="-DFM_ROLE_TESTTX=1" pio run -t upload --upload-port COMx
```

---

## Status

Verified on hardware, 9 September 2026:

- OLED, 2×2 matrix keypad, battery sense, buzzer
- Microphone capture → Codec2 1200 bps → LittleFS → decode → speaker
- Two-node LoRa link, PSK-authenticated alarms, **8/8** fragment reassembly
- Measured airtime **2599 ms** per 10-second note against **2556 ms** predicted

Codec2 runs at 41% of real time encoding, 31% decoding — comfortably ahead of
live capture on one core.

### Known work remaining

- **The PSK in `platformio.ini` is a public placeholder.** Alarm authentication
  is decorative until a real per-colony key is set.
- Transmitting blocks the UI for the length of a CAD backoff. The radio needs
  its own FreeRTOS task; the protocol was made correct single-threaded first so
  a later concurrency bug stays distinguishable from a protocol bug.
- Microphone input clips on loud syllables — gain needs backing off.
- Range and through-concrete performance untested.
- Buzzer SPL at 3.3 V unmeasured; it is the one part that must work when
  everything else has failed.

---

## Layout

```
include/    module interfaces, and floodmesh_pins.h — the pin map, with
            compile-time asserts that reject flash pins, UART pins, strapping
            pins and duplicate assignments
src/        implementation
docs/       hardware notes, decisions, and what bring-up actually found
```

The pin map is the single source of truth for wiring. It fails the build rather
than the board.
