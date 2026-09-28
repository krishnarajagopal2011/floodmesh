# FloodMesh field logger

A small web app that collects what every FloodMesh unit hears and sends during
field tests, so no reading depends on someone writing it down.

- Each unit (A–E) uploads its log every 5 minutes over the volunteer's phone
  hotspot (`POST /api/ingest`, signed with the fleet log key).
- Each volunteer opens the app on their phone, logs in as their unit, and sees
  the last upload time (red after 10 minutes of silence), battery and the
  latest records (dBm, SNR, hops, texts). With their consent the page shares
  the phone's GPS position, which stands in for the unit's position.
- The test lead logs in as admin: every unit at a glance, phone positions on a
  map, and CSV exports for the analysis, including **readings with location**
  (every received frame with both phones' positions and the distance).

Every record is stored with a timestamp and kept once per `(unit, seq)`, so
re-sent uploads never create duplicates. A *different* record under a seq
already stored (a board's flash erased, or a spare board given the unit's
call sign) is kept apart as a seq collision rather than dropped, and the unit
is flagged on the admin page. The contract with the firmware is
[`docs/field-logger-protocol.md`](../../docs/field-logger-protocol.md); change
that first, then both sides.

This is a **test tool**, not part of the product (the product does not use WiFi).

## How it is built

| Part | What |
|---|---|
| Framework | Next.js 16 (App Router, TypeScript), deployed on Vercel |
| Database | Neon Postgres when `DATABASE_URL` is set (Vercel); otherwise PGlite, an embedded Postgres stored in `./.pglite` (local development and tests). Same SQL for both. |
| Tables | `records` (one row per unit record, with the uploading board's `mac`), `record_conflicts` (seq collisions), `uploads` (one row per ingest request), `locations` (phone positions), `notes`, `login_limits` (rate limiting). Created automatically on first use; see `lib/schema.ts`. |
| Auth | One passcode per unit plus an admin passcode; HMAC-signed, httpOnly session cookie (12 h). A unit login sees and writes only its own unit. |
| Map | Leaflet with OpenStreetMap tiles |

Pages: `/login`, `/unit` (volunteer), `/admin`. API: `/api/ingest` (units),
`/api/login`, `/api/logout`, `/api/status`, `/api/overview`, `/api/location`,
`/api/note`, `/api/export/<name>.csv`.

## Settings

All settings are environment variables (template: [`.env.example`](.env.example)).

| Variable | Required | Meaning |
|---|---|---|
| `DATABASE_URL` | on Vercel | Neon connection string. Set by the Neon integration. Unset locally = PGlite. |
| `LOG_KEY` | yes | Fleet log key, 32–64 hex characters. Same value as the firmware build flag `FM_LOG_KEY`. |
| `SESSION_SECRET` | yes | Signs login cookies, at least 32 random characters. |
| `UNIT_PASSCODES` | yes | JSON, one passcode per unit: `{"A":"...","B":"...",...}` |
| `ADMIN_PASSCODE` | yes | The test lead's passcode. |
| `UNITS` | no | Allowed call signs, default `A,B,C,D,E`. |
| `TIMEZONE` | no | For "today" counts and the `ts_local` CSV column, default `Asia/Kolkata`. |
| `TRUST_PROXY` | no | `1` only when a self-hosted server sits behind a reverse proxy that appends the client address to `X-Forwarded-For`. Not needed on Vercel. See "Security notes". |

Generate secrets (any machine with Node):

```sh
# LOG_KEY: 64 hex characters (also goes into the firmware build as FM_LOG_KEY)
node -e "console.log(require('crypto').randomBytes(32).toString('hex'))"

# SESSION_SECRET
node -e "console.log(require('crypto').randomBytes(32).toString('base64url'))"

# UNIT_PASSCODES and ADMIN_PASSCODE: 8-digit numbers are easy to type on a phone
node -e "const c=require('crypto');const p={};for(const u of 'ABCDE')p[u]=String(c.randomInt(1e7,1e8));console.log(JSON.stringify(p));console.log('admin',c.randomInt(1e7,1e8))"
```

Keep them out of the repository (like the WiFi-update password): `.env*` files
are git-ignored except `.env.example`.

## Local development

Needs Node.js 22.18 or newer (Node 24 recommended).

```sh
cd webapp/field-logger
npm install
cp .env.example .env.local      # then fill in the values (see "Generate secrets")
npm run dev                     # http://localhost:3000
```

Leave `DATABASE_URL` out of `.env.local` and the data goes to `./.pglite`
(delete the folder to start again). Only one process may open that folder at
a time.

Example `.env.local` for a local test:

```sh
LOG_KEY=<64 hex characters>
SESSION_SECRET=<random string>
UNIT_PASSCODES={"A":"11111111","B":"22222222","C":"33333333","D":"44444444","E":"55555555"}
ADMIN_PASSCODE=<admin passcode>
```

### Simulator

`scripts/simulate.mjs` plays units A–E uploading a realistic 20-minute log
(boot, status, rx with RSSI/SNR/hops, echo, tx, relay, SOS state changes, some
records with `epoch = 0`, an unknown kind and field), then checks the answers:
re-sent batch stores nothing twice, a reused seq from a spare board or an
erased one is kept as a collision, wrong-typed fields and fields named like
`constructor` or `__proto__` land in `extra`, bad signature → 401, unknown
unit → 401, oversized body → 413, malformed JSON or common field → 400. It
prints PASS/FAIL per check and exits non-zero on any failure.

```sh
npm run simulate -- --url http://localhost:3000 --key <LOG_KEY>
```

With `--env .env.local` it takes the key, passcodes and admin passcode from the
file and also checks logins, location sharing, access control and the CSV
exports (row counts, time rules, distances):

```sh
npm run simulate -- --url http://localhost:3000 --env .env.local
```

On Windows, pass `--passcodes` as `A:1111,B:2222` rather than JSON; `cmd`
strips the quotes out of JSON arguments.

### Bench test with real units on the LAN

```sh
npm run build
npm start                       # listens on every interface, port 3000
```

Build the firmware with `FM_LOG_URL=http://<this PC's LAN IP>:3000` and the
same `FM_LOG_KEY` as `LOG_KEY`, or set it on a unit with the serial command
`log url http://<ip>:3000`. Allow port 3000 through the PC's firewall.

### Database schema

The app applies the schema on first use. To apply it explicitly (for example to
check a new Neon database before a field day, with `DATABASE_URL` in
`.env.local`):

```sh
npm run db:migrate
```

## Deploying on Vercel

1. **Push the branch** that contains `webapp/field-logger` to GitHub.
2. In the Vercel dashboard: **Add New… → Project → Import** the FloodMesh
   repository.
3. On the configure screen set **Root Directory** to `webapp/field-logger`.
   The framework is detected as Next.js; keep the default build and install
   commands.
4. Open **Environment Variables** (on the same screen, or later under
   *Settings → Environment Variables*) and add `LOG_KEY`, `SESSION_SECRET`,
   `UNIT_PASSCODES`, `ADMIN_PASSCODE` and, if not A–E, `UNITS`. Generate them
   as shown above. Deploy.
5. **Add the database:** in the project, open the **Storage** tab →
   **Create Database** → **Neon** (Serverless Postgres). Pick the region
   closest to the test site (AWS Asia Pacific, Mumbai `ap-south-1`) and connect
   it to the project for all environments. This sets `DATABASE_URL`.
6. **Pick the function region** next to the database: *Settings → Functions →
   Function Region* → Mumbai (`bom1`).
7. **Production branch:** the repository's default branch is `main`, which
   does not contain this app. Under *Settings → Git → Production Branch*
   choose the branch that does (for example `9th-Sept-2026`).
8. **Redeploy** (*Deployments → … → Redeploy*) so the new variables and the
   database apply. Environment variable changes only take effect on a new
   deployment.
9. Open `https://<project>.vercel.app/login` and log in as admin.
10. Point the units at it: build the firmware with
    `FM_LOG_URL=https://<project>.vercel.app` and `FM_LOG_KEY=<LOG_KEY>`.

Notes:

- Units must reach `/api/ingest` without a Vercel login. Vercel's default
  *Deployment Protection* covers preview deployments but not the production
  domain, so give the units the production URL (or turn protection off for
  the deployment they use).
- To check the deployment end to end, run the simulator against it from a
  laptop: `npm run simulate -- --url https://<project>.vercel.app --key <LOG_KEY>`.
  It writes simulated records, so clear them before the field test in the
  Neon SQL editor (Vercel → Storage → your database → Open in Neon):
  `TRUNCATE records, record_conflicts, uploads, locations, notes, login_limits;`
- Neon's free plan (0.5 GB) holds roughly a million records; a field day with five
  units is a few thousand.

## Exports

All exports are admin-only, stream as CSV and accept `?unit=A`.

| File | Contents |
|---|---|
| `records.csv` | Every record: protocol fields, `ts` (computed time, UTC), `ts_local`, `ts_src` (`unit` = unit clock, `uptime` = computed from uptime, empty = unknown), `device_epoch` (the unit's own claim), `received_at`, `extra` (unknown fields, and known fields that arrived with the wrong type, as JSON), `mac` (the board that uploaded it) |
| `locations.csv` | Every shared phone position (fix time, lat, lon, accuracy, altitude) |
| `notes.csv` | Volunteers' notes |
| `conflicts.csv` | Seq collisions (protocol section 4): records that arrived under a `(unit, seq)` already stored with different content, same columns as `records.csv`. `mac` tells the boards apart. Empty unless the admin page warned about a unit. |
| `readings_with_location.csv` | Every `rx` record with the receiving unit's and the sending unit's nearest phone position within ±2 min, how far in time each was (`*_loc_dt_s`) and the haversine `distance_m`. `from` is the frame's originator, so for `hops > 0` the distance is to the originator, not to the relay heard; filter on `hops = 0` for link analysis. |

Record time follows protocol section 5: the unit's `epoch` when its clock was
set; otherwise, for a record from the same boot as the upload, receive time
minus the uptime elapsed since the record; otherwise unknown (`seq` still
orders it).

Text cells starting with `=`, `+`, `-`, `@`, tab or carriage return get a
leading apostrophe so spreadsheets never run them as formulas. Numeric cells
(such as `rssi = -97`) are written as plain numbers.

## Server behaviour the protocol leaves open

- The `/api/ingest` response also carries `inserted`, `duplicates` and
  `conflicts` (counts for that request). Units ignore them; the simulator
  uses them.
- A request with no records is accepted (`acked_seq: null`) and still counts
  as "last upload" for the unit.
- A request with more than 100 records, or a bad envelope or common record
  field, is refused with `400` and the path of the first problem (for example
  `records[3].seq: expected a non-negative integer`). A known field of the
  wrong JSON type is not refused: it is kept in `extra` and logged (protocol
  section 3). Texts up to 500 characters are stored even though units cap
  them at 60.
- A request whose records were neither stored nor found (the same seq twice
  in one request, or two requests for one unit racing) is answered `500`
  without an acknowledgement; the unit resends it next window and the retry
  sorts it out.
- For `epoch = 0` records of the request's boot whose `ms` is later than the
  request's `up_ms`, the time is left unknown rather than set in the future.

## Security notes

- Uploads: HMAC-SHA256 over the raw body bytes, compared in constant time,
  checked before the body is parsed; the unit must be in `UNITS` and match
  both `X-FM-Unit` and `body.unit`. The protocol gives `FM_LOG_KEY` as a hex
  string without saying whether the HMAC key is its decoded bytes or its
  text; the server accepts either.
- Logins: passcodes compared in constant time. Every attempt is counted in
  the database before the passcode is compared, with one atomic upsert per
  key, so the limit holds across Vercel instances and under a burst of
  parallel guesses; a correct passcode gives its count back. Per IP: 10 wrong
  passcodes per 15 min. Per account: after 30 wrong passcodes in 15 min, only
  callers whose IP has not tried in that window may still try, so a guessing
  burst cannot lock a volunteer out of their own unit.
- The per-IP limit needs the real client address. On Vercel it comes from
  `X-Forwarded-For`, which Vercel sets. `next start` on its own passes a
  client's `X-Forwarded-For` through unchanged, so off Vercel the header is
  ignored and all callers share one bucket, unless `TRUST_PROXY=1` says a
  reverse proxy in front appends the address (the last entry is then used).
- A failed login redirects to `/login?error=<code>`; the page shows only its
  own text for a known code, never text from the link.
- Session cookie: httpOnly, SameSite=Lax, Secure on HTTPS, 12 hours. POSTs
  from other sites are refused (Origin check).
- Location is only sent after the volunteer agrees on the page, only while the
  page is open, and never by the admin login. The agreement holds for one unit
  and one login on that browser, and is forgotten on logout.
