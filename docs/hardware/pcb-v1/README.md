# Custom PCB — Rev V1.0

The purpose-built FloodMesh board that replaces the Heltec WiFi LoRa 32 V3
dev-board build described in [`../../hardware-notes.md`](../../hardware-notes.md).

| | |
|---|---|
| Client | Vazworks |
| Designed by | NSquare Bros |
| Revision | V1.0, 22-09-2026 |
| Board | 70 × 70 mm, 4 × Ø3.2 mm mounting holes |
| Source files | [`flood-mesh-v1.0-schematic-layout.pdf`](flood-mesh-v1.0-schematic-layout.pdf) (4 schematic sheets, layout, 3D views) · [`flood-mesh-v1.0-bom.pdf`](flood-mesh-v1.0-bom.pdf) (70 parts) |

This file is a reading aid for the PDFs. If the two disagree, the PDFs win —
and this file should be fixed.

---

## Main parts

| Ref | Part | Role |
|---|---|---|
| U1 | ESP32-S3-WROOM-1U-N16R8 | MCU — 16 MB flash, 8 MB octal PSRAM, U.FL for an external antenna |
| IC1 | Seeed Wio-SX1262 (114993390) | LoRa module; its RF switch is driven by the MCU (`RF_SW`) |
| IC2 | MAX17048G+T10 | Fuel gauge — real state of charge over I²C, `ALRT` interrupt to the MCU |
| IC3 | TPS6282533DMQR | 3.3 V buck, `EN` tied to `VIN` (always on) |
| U4 | MCP73833T-FCI/UN | Li-ion charger; `PROG` = 1 kΩ → about 1 A charge current |
| U2 | MAX98357A | I²S class-D amp → speaker on J2; ferrite + 470 pF output filter |
| MIC1 | INMP441 module | I²S MEMS mic, `L/R` tied to GND |
| LS1 | CMT-0904-85T | Magnetic buzzer — needs a PWM tone, not DC. BC847B low side from 3V3, SM4007 flyback |
| U3 | 0.96" 128×64 OLED | I²C, shared with the fuel gauge |
| S1 | TS11 right-angle | Side button (`BUTTON0`) |
| S2–S5 | TS04 tactile | Four face buttons (`BUTTON1`–`BUTTON4`) — one GPIO each, 10 kΩ pull-up + 100 nF. **Not a matrix.** |
| S6 | SWS045 slide switch | Disconnects the loads (`BATT_IN`); the charger stays on `BATT+` |
| J1 | USB-C (USB4730) | **Charging only.** 5.1 kΩ CC pull-downs, D+/D− not connected |
| J3 | JST-PH 2-pin | 1S LiPo, 3.7 V, 2000+ mAh; ESDA7P60 TVS (D3) across it |
| D4 | Red/green LED | Charge status, lit from VBUS only — no battery drain |
| P1/P2 · P3/P4 | DNP pads | Boot strap (`IO0` to GND) · UART0 TX/RX — the only programming path |

## GPIO map

| GPIO | Net | | GPIO | Net |
|---|---|---|---|---|
| IO0 | Boot strap (P1/P2) | | IO15 | `MIC_WS` |
| IO1 | `SDA` (OLED, fuel gauge) | | IO16 | `MIC_SCK` |
| IO2 | `SCL` | | IO17 | `MIC_DI` |
| IO3 | Strap (R4/R5 DNP) | | IO18 | `SPK_SD` (100 kΩ pull-down) |
| IO4 | `SPK_BCLK` | | IO20 | `LORA_RST` |
| IO5 | `SPK_LRCLK` | | IO21 | `ALERT` (fuel gauge) |
| IO6 | `SPK_DAT` | | IO38 | `LORA_DIO1` |
| IO7 | `BUZZER` | | IO39 | `LORA_BUSY` |
| IO8 | `RF_SW` | | IO40 | `LORA_MISO` |
| IO9 | `LORA_NSS` | | IO41 | `LORA_MOSI` |
| IO10 | `BUTTON4` | | IO42 | `LORA_SCK` |
| IO11 | `BUTTON3` | | | |
| IO12 | `BUTTON1` | | | |
| IO13 | `BUTTON2` | | | |
| IO14 | `BUTTON0` (side) | | | |

Unconnected: IO19, IO45–IO48. IO35–IO37 are taken by the octal PSRAM on the
N16R8 and must stay unconnected.

This map is completely different from the Heltec map in
[`include/floodmesh_pins.h`](../../../include/floodmesh_pins.h). The firmware needs
a separate board variant, not an edit to the existing one.

---

## What the board means for the network design

- **Real battery percentage.** The MAX17048 reports state of charge directly,
  which is the input battery-weighted relaying needs. A noisy ADC reading of a
  flat LiPo curve would not do.
- **Wake from deep sleep on any button.** IO10–IO14 are RTC GPIOs, so an SOS
  press can wake the chip from deep sleep. The same holds for the fuel-gauge
  `ALERT` on IO21 (low-battery wake).
- **The radio cannot wake the MCU from deep sleep.** `LORA_DIO1` is on IO38,
  and only IO0–IO21 are RTC-capable on the S3. Waking on received packets
  needs light sleep, which draws more current.
- **No 32.768 kHz crystal.** The S3's `XTAL_32K` pins (IO15/IO16) are used by the
  mic, so the sleep timer runs from the internal RC oscillator. Scheduled
  listening windows have to be wider to absorb the drift.
- **No GPS.** Location comes from the address registry.

## Open concerns for the next revision

1. **No battery temperature protection while charging.** The MCP73833 `THERM`
   pin is tied to a fixed 10 kΩ instead of an NTC on the cell. A LiPo charging
   inside a sealed enclosure in direct sun needs one. Highest priority.
2. **No reverse-polarity protection on J3.** JST-PH LiPo packs from different
   vendors are wired both ways round. Fix with a P-FET, or buy packs from one
   vendor only.
3. **`LORA_DIO1` on a non-RTC pin** (see above). Moving it to IO19 — free,
   and USB data is not used — would allow deep-sleep wake on radio activity.
4. **Fuel gauge after the power switch.** It re-learns the cell after every
   power-on, so readings are less accurate for a while each time. On the
   `BATT+` side its hibernate current is only a few µA.
5. **Programming only through unpopulated UART pads.** There is no USB data
   path, so plan how firmware updates will be done in the field.
6. **Buzzer loudness at 3.3 V is unmeasured.** This was already an open item on
   the dev-board build.
