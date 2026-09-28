# Field test, 28 September 2026: plan and status

For anyone (including a Claude session on a phone) answering questions during
or after today's test. The volunteer instructions are in
`docs/field-tests/FloodMesh-team-guide.pdf`; results go into
`docs/field-tests.md` afterwards.

## Units and roles

| Unit | Role | Firmware on the unit | Notes |
|---|---|---|---|
| A | Person in trouble: sends the SOS, range ping on | V4 4.2.0 (`v4`, SF7) | Header `A CIV` (a `USER` label build exists, not yet installed) |
| B | Walker: reads the Status screen every ~100 m | V4 4.2.0 | |
| C | **Roof tower**: on the building roof, on a power bank, power mode forced ON (Status → `#` → `PWR ON`) | V4 4.2.0 | A powered unit never sleeps and forwards everything (architecture §13.2) |
| E | Rescuer: receives SOS, replies "Help is coming" (hold 4 + 6 for 3 s) | **Test-only** env `v4_bench_responder` (header `E RSP TEST`) | Acts as a responder without Admin-app registration; never give this build to users |
| D | Stays home | V4 4.2.0 | Battery reads ~2.85 V even on USB: cell probably dead |

All units: SF7, 125 kHz, 20 dBm, 866.5 MHz. V4 civilian units forward SOS
silently; only E shows it.

## What volunteers record

- Web app **floodmesh-field.vercel.app**: each volunteer logs in with their
  unit letter and passcode, shares phone location (with consent, page open,
  screen on) and types readings into Notes. Every note gets a server
  timestamp and the phone's position. The red "Nothing received yet" box is
  expected today: units cannot upload yet (see below).
- WhatsApp group: one line per reading, in the formats in the team guide, e.g.
  `B | near temple | A: hop 0, -97, SNR 3 | C: hop 0, -88, SNR 8`, plus photos
  of the Status screen.
- Reading guide: dBm closer to 0 is stronger (edge about −120); SNR above 0 is
  good, about −7 is the SF7 limit; hop 0 = direct, 1 = through one unit.

## Field logger status (automatic uploads)

- Protocol: `docs/field-logger-protocol.md`.
- Web app: `webapp/field-logger` (Next.js + Neon Postgres), **deployed** to
  Vercel project `floodmesh-field`. Secrets (log key, session secret, unit and
  admin passcodes) are only on the owner's PC and in Vercel, never in git.
- Unit firmware (`fm_log`, uploads every 5 min over WiFi): built and reviewed,
  fixes in progress, **not on any field unit today**. Next step: test on D over
  home WiFi, then install on all units over WiFi update mode.
- Volunteers' phone hotspots will all be named **`floodmesh`** (same password,
  2.4 GHz, WPA2); units store it as their second network
  (`set_wifi.py --slot 2`). Not used today.

## Quick answers

- **E shows no SOS**: check E's header says `RSP TEST`; check A appears in E's
  Status list (Home → B). A civilian unit never shows an SOS in V4.
- **A shows `SOS NOT DELIVERED`**: no responder heard it after 5 retries
  (1, 2, 4, 8, 15 min). Move closer or check C's power bank.
- **C must stay powered**: on battery a V4 unit sleeps and forwards texts only
  when it has not heard a powered unit; SOS, ACKs and heartbeats are always
  forwarded.
- **Screen dark**: press D. **Never hold keys while switching on** (`#`
  Bluetooth setup, `B` WiFi update mode, `*` + `0` for 10 s factory reset).
