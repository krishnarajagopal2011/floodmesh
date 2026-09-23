# Custom PCB V1.0 designed

Tags: #announcement
Channels: X, Instagram (carousel), LinkedIn
Visual: screenshot of the 3D board view from
`docs/hardware/pcb-v1/flood-mesh-v1.0-schematic-layout.pdf`, plus a page of
the schematic sheets. No photo of a built board yet: it has not been
fabricated.

## Facts

- Client Vazworks, designed by NSquare Bros, Revision V1.0 dated 22-09-2026.
  docs/hardware/pcb-v1/README.md.
- Board 70 x 70 mm, 4 x 3.2 mm mounting holes. docs/hardware/pcb-v1/README.md.
- Not yet fabricated. docs/hardware/pcb-v1/README.md and docs/architecture.md
  intro ("custom PCB, not yet fabricated").
- MCU: ESP32-S3-WROOM-1U-N16R8, 16 MB flash, 8 MB octal PSRAM, U.FL for an
  external antenna. docs/hardware/pcb-v1/README.md main parts table.
- LoRa: Seeed Wio-SX1262 module. docs/hardware/pcb-v1/README.md.
- Fuel gauge: MAX17048G+T10, real state of charge over I2C, replacing an ADC
  battery-voltage read. docs/hardware/pcb-v1/README.md, "What the board
  means for the network design".
- Replaces the Heltec WiFi LoRa 32 V3 dev-board build. docs/hardware/pcb-v1/README.md
  intro; dev-board keypad was a 2x2 matrix, README.md GPIO table.
- Keypad change: 4 face buttons (S2-S5), each its own GPIO, explicitly "Not
  a matrix", plus one side button. docs/hardware/pcb-v1/README.md main parts
  table.
- IO10-IO14 are RTC-capable, so a button press can wake the chip from deep
  sleep. docs/hardware/pcb-v1/README.md, "Wake from deep sleep on any
  button".
- Open concern: no reverse-polarity protection on the 2-pin JST battery
  connector; the fix on the table is a P-FET, or sourcing packs from one
  vendor only. docs/hardware/pcb-v1/README.md, "Open concerns for the next
  revision", item 2.
- Also open and higher priority: no battery temperature protection while
  charging (fixed 10 kOhm instead of an NTC on the cell). Not used in this
  draft's posts but kept here for the next one. docs/hardware/pcb-v1/README.md
  item 1.

## X thread (each post under 280 characters, checked)

**1/** (274 chars)
#announcement

We are building an off-grid pager for floods. Our first custom PCB (V1.0)
just landed. Not fabricated yet, design is done.

70x70 mm board. ESP32-S3 with 16 MB flash, 8 MB PSRAM, a Wio-SX1262 LoRa
module, and a real fuel gauge chip for actual battery percent.

[image: 3D board view from the layout PDF]

**2/** (197 chars)
Biggest change from our dev-board build: the 2x2 matrix keypad becomes 4
separate buttons, each its own GPIO. Every one of them can wake the chip
from deep sleep, so an SOS press works even asleep.

**3/** (256 chars)
Open question before we send this to fab: the battery connector has no
reverse-polarity protection yet. A P-FET, or just buy LiPo packs from one
vendor only? What would you do here?

Open hardware, built in public: github.com/krishnarajagopal2011/floodmesh

## Instagram caption

#announcement

We are building an off-grid pager for floods. Our first custom PCB just
landed, not fabricated yet, but the design is done.

70x70 mm board built around an ESP32-S3 with 16 MB flash and 8 MB PSRAM, a
Wio-SX1262 LoRa module, and a fuel gauge chip that reports real battery
percentage instead of an ADC guess. It replaces the Heltec dev board we
have been building on.

The keypad changes too: the old 2x2 matrix becomes 4 separate buttons, one
GPIO each. Every one of them can wake the chip from deep sleep, so pressing
SOS works even if the unit was asleep.

Before this goes to fab, one thing is still open: the battery connector has
no reverse-polarity protection. Fix it with a P-FET, or just source packs
from a single vendor?

Why: in Chennai 2015 and during Cyclone Michaung, towers and power failed
together. Neighbours 200 m apart could not reach each other.
FloodMesh hops alarms and 10-second voice notes around buildings, for days on
common lithium cells.
Open hardware, built in public: link in bio.

#floodmesh #pcbdesign #esp32 #lora #openhardware #hardwaredesign #buildinpublic #disastertech

## LinkedIn

Our first custom PCB for FloodMesh, an off-grid emergency pager for urban
floods, just came back from design. It has not been fabricated yet, but the
board is done: 70x70 mm, an ESP32-S3 with 16 MB flash and 8 MB PSRAM, a
Wio-SX1262 LoRa module, and a fuel gauge chip for real battery percentage
instead of a rough voltage read. It replaces the Heltec dev-board build we
prototyped on.

One deliberate change: the keypad moves from a 2x2 matrix to 4 separate
buttons, each on its own GPIO. Every button is wake-capable from deep
sleep, so an SOS press works even when the unit has been sitting idle.

One thing is still open before this goes to fabrication: the battery
connector has no reverse-polarity protection. The candidates on the table
are a P-FET on the input, or restricting battery packs to a single vendor.

If you have designed or manufactured small-batch consumer electronics in
India and have a view on either of those, I would like to hear it.

github.com/krishnarajagopal2011/floodmesh

#openhardware #pcbdesign #disastertech
