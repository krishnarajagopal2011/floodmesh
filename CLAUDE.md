# FloodMesh — notes for Claude sessions

## Hardware
Two boards exist. Check which one a change targets.
- **Heltec WiFi LoRa 32 V3.2 dev-board build.** The current firmware pin map is
  `include/floodmesh_pins.h`; the write-up is `docs/hardware-notes.md` and `docs/wiring-as-built.md`.
- **Custom PCB Rev V1.0** (client: Vazworks, designed by NSquare Bros, 22-09-2026):
  `docs/hardware/pcb-v1/`. Read its `README.md` (parts, GPIO map, design
  implications, open concerns) before suggesting features or components. The
  schematic, layout and BoM PDFs are next to it.

## Regulatory
India, 865–867 MHz licence-exempt band. The firmware assumes G.S.R. 853(E) (2021)
Table II: 2.5% duty cycle, 125 kHz bandwidth. This has not been verified against
the gazette text yet, and third-party summaries disagree.

## Network architecture: under discussion, nothing final
Tentative direction from the design discussion:
- Civilian units deep-sleep and send short preset distress messages. Voice is
  mainly for responders; voice capacity, not mesh flooding, is what saturates the channel.
- Location comes from a household address registry keyed by call sign, cross-checked
  against which relay heard the message. No GPS.
- Fixed relays help but must never be required. Every life-safety path has to
  work with zero relays.
- When relays go quiet, units switch to mesh mode. Units above a battery threshold
  (about 50% on, 35% off) volunteer to listen and relay. There is no election:
  the rebroadcast delay is weighted by battery, floor and SNR, and the first
  unit to transmit suppresses the others.
- Relays and volunteers cache ACKs and replay them to sleeping senders that retry.
