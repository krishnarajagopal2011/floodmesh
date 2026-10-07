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
- **Antenna (all builds):** every unit uses the same 868 MHz half-wave rubber
  duck on an SMA socket; per-role upgrades and rules are in
  `docs/architecture.md` §14.
- **Prototype v2 perfboard build** (15 Heltec V3 units, 4×4 keypad via MCP23017, SOS
  button, power sense, mic on Vext): `docs/prototype-v2-build.md`. Firmware
  envs `proto_v2` and `proto_v2_selftest` (`-D FM_BOARD_PROTO_V2=1`).
- **Firmware V3** (`firmware_v3/`, a separate PlatformIO project): Heltec V3 +
  4×4 membrane keypad wired straight to GPIOs (cols 4–7, rows 39–42) + active
  buzzer on GPIO 2. No voice. This is the field-test build for range, relays,
  60-char T9/multi-tap texts (frame type 0x03), SOS (hold * + #) and BLE roles.
  Build: `pio run -d firmware_v3 -e v3`. Read `firmware_v3/README.md`. It
  copies modules from the root firmware; don't edit the root firmware for V3
  changes, or the reverse.
- **Firmware V4** (`firmware_v4/`, a separate PlatformIO project, same
  hardware and wiring as V3): the 27 Sep decisions (`docs/architecture.md`
  §13) - SOS channel screen, SOS shown only on responder units with channel
  tabs, delivery ACK / "Help coming" frames (0x04), SOS retries, powered-unit
  heartbeats (0x05) and the text-forwarding rule, light sleep with DIO1/keypad
  wake, SF as a build setting, OTA trial + rollback, and an optional
  5V -> GPIO 3 power-sense divider (probed at boot). Envs `v4`, `v4_sf9`,
  `v4_wake`. Build: `pio run -d firmware_v4 -e v4`. Read
  `firmware_v4/README.md`. V3 stays as it is for comparison; keep V3 and V4
  changes separate.

## Firmware and app
- Build with PlatformIO locally, or let `.github/workflows/build.yml` build
  all envs and the Android APK. This cloud sandbox cannot build either: its
  network policy blocks the PlatformIO registry and dl.google.com (see
  "Cloud sandbox network" below). raw.githubusercontent.com and
  api.github.com are reachable: use them to read library headers and CI run
  status.
- **Coverage planner** (`webapp/coverage-planner/`, Next.js like the field
  logger): plans unit placement, coverage and cost over OpenStreetMap by
  constituency, locality or drawn area; `/admin` (password) edits the cost
  sheet. The radio model in `lib/radio.ts` is fitted to `docs/field-tests.md`;
  refit it when new field results come in. Read its `README.md`.
- **Public website** (`website/`, static, deployed on Vercel with Root
  Directory `website`, on `floodmesh.tech` plus
  `floodmesh-website.vercel.app`): home page, pre-order store (no payment
  yet; orders go through `api/preorder.js` to a Google Sheet via
  `apps-script/`), privacy and
  pre-order terms, in English with Tamil and Hindi. Read its `README.md`
  first: it has the content rules (no technical detail that helps copying,
  claim only measured results, real photos for the product) and the go-live
  checklist. Prices live in `assets/js/products.js` and `apps-script/Code.gs`.
- BLE role registration: protocol `docs/ble-provisioning-protocol.md`, unit side
  `src/fm_prov.cpp` + `src/fm_role.cpp`, admin app `app/floodmesh_admin/`
  (Flutter). Change the protocol doc first, then both sides.

## Regulatory
India, 865–867 MHz licence-exempt band. The firmware assumes G.S.R. 853(E) (2021)
Table II: 2.5% duty cycle, 125 kHz bandwidth. Not yet verified against the gazette
text. The current firmware's duty-cycle exemption for alarms is a legal risk;
see `docs/architecture.md` §1.1. The 2021 rules for 865–868 MHz may also
require adaptive transmit power control (§1.1, unverified). What must be
certified before sale (WPC ETA, BIS for the battery, CPCB EPR) is in
`docs/certification-india.md`.

## Cloud sandbox network
The environment's network policy blocks some hosts this project needs. The
owner changes it in the cloud environment menu in the session's title bar →
Edit → Network access: a broader access level, or these hosts added to the
allowed domains (levels: https://code.claude.com/docs/en/claude-code-on-the-web).

| For | Hosts |
|---|---|
| Gazette and regulatory text | `dot.gov.in`, `dms.dot.gov.in`, `thc.nic.in`, `egazette.gov.in`, `tec.gov.in`, `saralsanchar.gov.in` |
| Legal and consultancy summaries | `legalitysimplified.com`, `legitquest.com`, `pcnindiaglobal.com`, `sunren.net`, `thejeshgn.com` |
| Local firmware and app builds | the PlatformIO registry, `dl.google.com` |
| Coverage planner maps and search (browser tests) | `tile.openstreetmap.org`, `nominatim.openstreetmap.org`, `overpass-api.de`, `server.arcgisonline.com`, `*.basemaps.cartocdn.com` |
| Vercel deploys from the sandbox (also needs a `VERCEL_TOKEN` environment variable) | `vercel.com`, `api.vercel.com`, `*.vercel.app` |
| AI images for the website from Hugging Face (also needs Spaces switched on at huggingface.co/settings/mcp) | `huggingface.co`, `*.hf.space`, `*.hf.co` |

Until they are allowed, ask the owner to download a gazette PDF and commit it
to `docs/reference/`.

## Architecture in one paragraph
Same hardware for every role. Civilian units work out of the box (optional
registration via a Flutter app with OTP), deep-sleep, and wake by radio: the
SX1262 wakes the ESP32 when a packet with a wake-up preamble arrives (no shared
listening window). They send an SOS with a channel (side button, or * + # on the
4×4 keypad) and 60-character text (T9 English, multi-tap Tanglish, no Tamil
script); no voice in v1. Every unit forwards SOS, which only responder units
show; text shows on every unit. There is no relay role: any unit on external
power stays awake, forwards everything and sends responders a heartbeat every 30
min; battery units forward text only when no powered unit is nearby (no
election; the first to transmit wins a weighted-delay race). Location comes from
a call-sign → address registry, a zone cross-check and hot/cold homing, with no
GPS. Firmware updates go over Bluetooth from the app, signed, with rollback. One
spreading factor for the whole network, chosen by field tests. Responders are
made by one super admin over Bluetooth, sign messages with their own key, and
expire after 10 days. Details and caveats are in `docs/architecture.md`.

## Branches
`9th-Sept-2026` holds the PlatformIO firmware and these docs. `main` has an
unrelated older history (single `.ino` plus `docs/ARCHITECTURE.md`); the two
share no commits, so don't merge them blindly.
