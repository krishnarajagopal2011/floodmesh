# FloodMesh — notes for Claude sessions

The repository owner is the project owner (not building for a third party).
Treat them as the decision-maker.

## Read first
- **`docs/architecture.md`**: every network-architecture decision made so far,
  with its reasoning, the numbers behind it, rejected alternatives, the PCB
  change list, implied firmware changes and open questions. Items are marked
  DECIDED / PROPOSED / OPEN. Don't re-open DECIDED items without a new reason,
  and don't treat PROPOSED items as agreed.

## Hardware
Two boards exist. Check which one a change targets.
- **Heltec WiFi LoRa 32 V3.2 dev-board build.** The current firmware pin map is
  `include/floodmesh_pins.h`; the write-up is `docs/hardware-notes.md` and `docs/wiring-as-built.md`.
- **Custom PCB Rev V1.0** (title block: Vazworks / NSquare Bros, 22-09-2026,
  **not yet fabricated**): `docs/hardware/pcb-v1/`. Read its `README.md`
  (parts, GPIO map, design implications, concerns) before suggesting features
  or components. Pending board changes are in `docs/architecture.md` §9.
- **Prototype v2 perfboard build** (15 Heltec V3 units, MCP23017 keypad, SOS
  button, power sense, mic on Vext): `docs/prototype-v2-build.md`. The firmware
  pin map has not yet been updated to match it.

## Regulatory
India, 865–867 MHz licence-exempt band. The firmware assumes G.S.R. 853(E) (2021)
Table II: 2.5% duty cycle, 125 kHz bandwidth. Not yet verified against the gazette
text. The current firmware's duty-cycle exemption for alarms is a legal risk;
see `docs/architecture.md` §1.1.

## Architecture in one paragraph
Same hardware for every role. Civilian units work out of the box (optional
registration via a Flutter app with OTP), deep-sleep, and wake for a shared
listening window. They send one-press presets, a side-button SOS to responders,
and 60-character text from a 12-key keypad (T9 English, multi-tap Tanglish, no
Tamil script). Location comes from a call-sign → address registry, a relay-zone
cross-check and hot/cold homing, with no GPS. At least 3 relays per 100
households, preferring solar/UPS hosts; relays help but are never required, and
civilian units fall back to a mesh mode where units above a battery threshold
volunteer to relay (no election; the first to transmit wins a weighted-delay
race). Responders are made by one super admin over Bluetooth, sign messages
with their own key, and expire after 10 days. Details and caveats are in
`docs/architecture.md`.

## Branches
`9th-Sept-2026` holds the PlatformIO firmware and these docs. `main` has an
unrelated older history (single `.ino` plus `docs/ARCHITECTURE.md`); the two
share no commits, so don't merge them blindly.
