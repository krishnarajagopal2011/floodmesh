# Field logger protocol (units → web app)

Contract between firmware V4's field logger (`firmware_v4/src/fm_log.cpp`) and
the field-logger web app (`webapp/field-logger/`). Both sides are built from
this file; change it first, then both.

Purpose: during field tests every unit records what it receives and sends, and
uploads it every 5 minutes over the volunteer's phone hotspot, so no reading
depends on someone writing it down. Volunteers open the web app on their phone
to see their unit's data and (with consent) share the phone's GPS position,
which gives distances for the analysis (units have no GPS).

This is a **test tool**, not part of the product: it runs over WiFi, which the
product does not use.

---

## 1. Transport

- `POST <base>/api/ingest`, where `<base>` is the web app's address, e.g.
  `https://floodmesh-field.vercel.app` or, for bench tests on the LAN,
  `http://192.168.1.42:3000`. The firmware supports both `https://` and
  `http://`.
- Headers:
  - `Content-Type: application/json`
  - `X-FM-Unit: <call sign>` (trimmed, e.g. `A`)
  - `X-FM-Signature: <hex>`: lowercase hex **HMAC-SHA256 of the raw request
    body bytes**, key = the fleet log key (§6).
- One request carries at most **100 records** and at most **16 KB** of body.
  The unit sends several requests in one upload window until it has nothing
  left or a request fails.
- HTTPS: the unit verifies the server certificate against the ESP32 Arduino
  core's certificate bundle when available; a build flag may fall back to
  unverified TLS for bench tests only. The HMAC protects integrity either way.

## 2. Request body

```json
{
  "v": 1,
  "unit": "A",
  "mac": "30:ED:A0:CB:2F:28",
  "fw": "4.2.0",
  "image": "2578431a",
  "boot": 17,
  "up_ms": 423001,
  "epoch": 1790563200,
  "dropped": 0,
  "records": [ ... ]
}
```

| Field | Type | Meaning |
|---|---|---|
| `v` | int | Protocol version, `1` |
| `unit` | string | Call sign without padding. Must equal `X-FM-Unit` |
| `mac` | string | Factory MAC, upper-case, colon-separated |
| `fw` | string | `FLOODMESH_VERSION` |
| `image` | string | First 8 hex digits of the image's ELF SHA-256 (as `/id` in WiFi update mode) |
| `boot` | int | Boot counter, persisted in NVS, +1 on every boot |
| `up_ms` | int | Unit uptime (ms) when the request was built |
| `epoch` | int | Unit's current Unix time (s) if its clock is set, else `0` |
| `dropped` | int | Records lost so far because the unit's log was full (lifetime counter) |
| `records` | array | Oldest first, see §3 |

## 3. Records

Every record has these fields:

| Field | Type | Meaning |
|---|---|---|
| `seq` | int | Per-unit sequence number, strictly increasing across reboots (persisted, gaps allowed) |
| `boot` | int | Boot counter when the record was made |
| `ms` | int | Uptime (ms) when the record was made |
| `epoch` | int | Unix time (s) when the record was made, or `0` if the clock was not set yet |
| `k` | string | Kind, below |

Kind-specific fields (absent when not applicable). Unknown kinds and fields
must be accepted and kept by the server (stored in `extra`). So must a known
field whose JSON type differs from the table (e.g. `"hops": "two"`, or a new
kind that uses `n` for a float): the server stores the record, keeps that
value in `extra` under its own name, lists the name in `extra._type_errors`
and logs it. Only the envelope and the five common fields above can make the
server refuse a request, because a refused request is resent unchanged every
window and would block every later record of that unit.

String fields are stored exactly as sent (only NUL characters are removed):
`text` keeps leading and trailing spaces, so it matches the unit's `RXLOG`
line.

| `k` | Fields | When |
|---|---|---|
| `boot` | `reason` (string, ESP reset reason), `fw`, `image` | Once per boot |
| `rx` | `t` (`SOS`, `ALARM`, `TEXT`, `PING`, `ACK`, `HEARTBEAT`), `from`, `id`, `hops`, `rssi` (int dBm), `snr` (float dB), `text` (≤ 60 chars, optional), `ch` (SOS channel 0–4, optional) | Every frame the unit receives and accepts, including ones it forwards silently |
| `echo` | `t`, `id`, `rssi`, `snr` | A neighbour was heard relaying one of this unit's own frames ("passed on") |
| `tx` | `t` (`SOS`, `TEXT`, `PING`, `ACK`, `HEARTBEAT`, `HELP`), `id`, `ok` (bool), `text` / `ch` optional | This unit transmitted its own frame (not relays) |
| `relay` | `t`, `from`, `id` | This unit relayed someone else's frame |
| `sos` | `state` (`SENT`, `RETRY`, `DELIVERED`, `HELP`, `NOT_DELIVERED`, `STOPPED`), `id`, `ch`, `n` (retry count) | This unit's own SOS changed state |
| `status` | `batt_v` (float), `batt_pct` (int), `powered` (bool), `air_pm` (int, airtime per mille), `heard` (int, stations in the heard list), `sleep_pct` (int), `role` (`civilian`/`responder`) | At boot, then every 5 min |

`rssi`/`snr` are exactly the values the firmware already prints in its
`RXLOG` serial line.

## 4. Response

`200` with

```json
{ "ok": true, "acked_seq": 1041, "server_epoch": 1790563230 }
```

- `acked_seq`: the highest `seq` in this request that is now stored (new,
  already present, or kept as a seq collision, below). The unit deletes
  records with `seq <= acked_seq`.
- `server_epoch`: server time (s). The unit uses it to set its clock if NTP
  failed.

Errors: `400` malformed JSON, envelope or common record fields (§3), `401`
bad signature or unit not allowed, `413` body over 16 KB, `5xx` server
trouble. On any non-200 the unit keeps the records and tries again next
window.

Re-sending records is safe: the server stores each `(unit, seq)` once.

**Seq collisions.** A record whose `(unit, seq)` is already stored counts as
a re-send only if everything the unit sent for it is identical (every record
field, and the envelope's `mac` when both uploads gave one). Otherwise it is a
different record under a reused `seq`. That happens when `seq` starts again
at 1: the board's flash was erased (`erase_flash`), a spare board was given
the unit's call sign, or the NVS write of the `seq` high-water mark failed.
The server then:

- keeps the record apart (table `record_conflicts`, export `conflicts.csv`),
  with the `mac` of the board that sent it, once per `(unit, seq, boot, ms,
  k, mac)` so a re-send of it is harmless too;
- still counts it as stored for `acked_seq`, so the unit deletes it. Refusing
  it instead would make the unit resend the same oldest-first batch every
  window, stalling its whole log;
- logs `[ingest] <unit>: N seq collisions (...)` and flags the unit on the
  admin page, which also lists every `mac` that has uploaded under each call
  sign.

Every stored record carries the `mac` of the upload that brought it, so
records from two boards under one call sign can be told apart in the
analysis. Records do not carry the call sign they were made under: records
still unsent when `callsign` is changed are uploaded under the new one.

## 5. Time

- When on WiFi the unit syncs its clock by SNTP (`pool.ntp.org`,
  `time.google.com`), or failing that from `server_epoch`. From then on records
  carry `epoch`.
- For a record with `epoch = 0` the server computes its time as
  `request_received_at − (up_ms − ms)` **when the record's `boot` equals the
  request's `boot`** (same boot, so uptimes are comparable). Otherwise the
  time is unknown (`NULL`); `seq` still orders it.
- The server stores both the unit's claim and the server receive time.

## 6. Keys and settings

| Setting | Where | Notes |
|---|---|---|
| Fleet log key | Build flag `FM_LOG_KEY` (hex string, 32–64 chars); web app env `LOG_KEY` | Same key for every unit. Without it the unit still records locally but never uploads (serial says so) |
| Server address | Build flag `FM_LOG_URL` (default), NVS key `logurl` (overrides) | Base URL without `/api/ingest` |
| Upload period | Build flag `FM_LOG_PERIOD_MS`, default `300000` | First upload about 60 s after boot |
| WiFi networks | NVS `wifissid`/`wifipass` (existing, shared with WiFi update mode) and `wifissid2`/`wifipass2` (second, e.g. the volunteers' hotspot) | The uploader tries network 1, then network 2 |

The log key is a test secret like the WiFi-update password: kept in a local
file outside the repository, passed at build time, never committed.

## 7. Unit behaviour (summary)

- Records are kept in flash (LittleFS) so they survive restarts and battery
  swaps; capacity ≥ 5,000 records; when full the oldest are dropped and
  `dropped` counts them.
- Uploading runs in its own FreeRTOS task, so the radio and the UI keep
  working. While it runs the unit does not light-sleep. It does not run while
  WiFi update mode or Bluetooth provisioning is active.
- WiFi is switched off again after each upload window.
- Serial commands: `log` (status: stored, sent, last result and age, server
  address), `log send` (upload now), `log url <base>`, `log clear`,
  `wifi2 ssid <name>`, `wifi2 pass <password>`, `wifi2 forget`; `wifi` shows
  both networks.
- The Status screen shows one line: records waiting and time since the last
  successful upload.

## 8. Web app (summary)

- Next.js on Vercel, Neon Postgres (`DATABASE_URL`); without `DATABASE_URL`
  (local development and tests) it uses an embedded Postgres (PGlite).
- Logins: one per unit (`A`–`E`, passcodes from env `UNIT_PASSCODES`, JSON) and
  one admin (`ADMIN_PASSCODE`). Signed session cookie (`SESSION_SECRET`).
- Volunteer page: the unit's last upload time (red when older than 10 min),
  battery, latest records (time, kind, from, hops, dBm, SNR, text),
  a notes box, and **Share my location** after an explicit consent step
  (browser geolocation, only while the page is open, posted at most every
  20 s).
- Admin page: all units at a glance (with a warning for seq collisions or a
  second board under one call sign, §4), latest phone positions on a map, and
  CSV exports: records, locations, notes, seq collisions, and **readings with
  location**: every `rx` record joined with the receiving unit's and the
  sending unit's nearest phone position (within ±2 min) and the distance
  between them.
