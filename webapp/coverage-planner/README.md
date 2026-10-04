# FloodMesh coverage planner

A web app for planning where FloodMesh units go and what a deployment costs,
over OpenStreetMap, for proposals to government bodies and organisations.

- **Map on the left, tools on the right.** Choose the area by **assembly
  constituency** (all 234 in Tamil Nadu, e.g. Alandur, Maduravoyal), by
  **locality** (OpenStreetMap search, e.g. Anna Nagar, Nandambakkam), or
  **draw** a polygon, rectangle or circle. GeoJSON import too.
- **Units.** Place powered terrace units and responders by clicking, drag them
  onto real terraces, or **auto-place** powered units so that every house hears
  one or two of them (§4.2 of `docs/architecture.md` asks for two). Household
  units are counted, not placed: by density, from OpenStreetMap building
  counts, or entered directly, times the share of households that get one.
- **Coverage.** A heat map of how many powered units a household unit indoors
  would hear, the signal margin, or how many relays its SOS needs to reach a
  responder. **Probe** any spot for the link budget to the nearest units.
  Checks flag gaps, islands, SOS beyond the hop limit and heartbeat airtime.
- **Cost.** An estimate from the admin's price list: units, survey, testing,
  certification, training, maintenance and contingency, with cost per household
  and per km².
- **Report.** Print or save as PDF (map, summary, checks, cost, assumptions).
  Save and open plans as files; export KML (Google Earth), GeoJSON (QGIS) and
  a CSV of unit positions.
- **Admin** (`/admin`, password protected). Edit the cost sheet: component
  lines per unit type, project lines with their basis (fixed, per km², per
  unit), markup and contingency. The planner only receives the resulting price
  per unit type and rate per project line; the breakdown and markup stay on
  the admin page.

Anyone with the link can use the planner; only the admin can change prices.
Plans live in the user's browser and in the files they save, not on the
server.

## How coverage is computed

Everything is in `lib/radio.ts` and `lib/plan.ts`, with tests in `test/`.

| Step | Model |
|---|---|
| Path loss | Log-distance, fitted to the FloodMesh field tests (`docs/field-tests.md`): PL(d) = 31.2 + 10·n·log₁₀(d) − G<sub>h</sub>(h₁) − G<sub>h</sub>(h₂) + clutter, never below free space. n = 4.2 from the 26 Sep test (350 m at −122 dBm gives 4.36, 300 m at −112 dBm 4.07, 250 m at −114 dBm 4.29). |
| Height gain | G<sub>h</sub>(h) = min(20·log₁₀(h / 1 m), 20 dB): two-ray, matching the field-test estimate of +15 dB at 6 m and +20 dB at 10 m. |
| Surroundings | Clutter offset: dense urban +6 dB, urban +3 dB (default), suburban 0 dB (the calibrated site), open −6 dB. |
| Link budget | TX + antenna gains − cable and wall losses − SX1262 sensitivity at the SF − fade margin (10 dB default, for the ±10 dB swings seen in the field). The weaker direction of a link counts. TX is capped by the e.r.p. ceiling (§14.2 rule 2). |
| Unit profiles | Household unit 1.5 m up indoors behind 12 dB of wall, 2.2 dBi whip. Powered unit 10 m up on a terrace, 6 dBi omni, 1 dB cable. Responder 1.5 m outdoors, whip. All editable; each placed unit can have its own height and antenna. |
| Coverage grid | About 40,000 cells over the area (10–500 m each). Each cell is a household unit indoors. |
| SOS reach | Breadth-first search from responders through powered units; a cell is reached if one of its powered units is within the hop limit (`FM_HOP_DEFAULT` = 3). Battery units also forward SOS (§13.3), so real reach is better. |
| Airtime | Semtech AN1200.13 time on air (125 kHz, CR 4/5, 16-symbol preamble) plus the 0.5 s radio-wake preamble (§13.1). Heartbeat load around the busiest powered unit, against the ~1,200 s/h usable channel (§1.3) and the 2.5% duty cycle (§1.1). |
| Auto-place | Triangular lattice (spacing = range for two-unit coverage, range·√3 for one), clipped to the area, then gaps filled greedily. |

With the defaults (SF7, urban): household ↔ powered unit about 650 m,
powered ↔ powered about 3.7 km, household ↔ household about 110 m.

This is a planning estimate. It has no terrain or building data, no flood
water and no interference from other users of 865–867 MHz. A walk test at
each powered site is what turns it into a plan; the Radio tab can fit n to a
new field reading.

## Data sources

| Data | Source | Licence / policy |
|---|---|---|
| Base maps | OpenStreetMap tiles; Esri World Imagery; CARTO light | Attribution shown on the map; OSM's tile policy forbids heavy use |
| Locality search | Nominatim (`nominatim.openstreetmap.org`) | At most 1 request per second, only when Search is pressed |
| Building count | Overpass API (`overpass-api.de`) | Fair use |
| Constituencies | `public/data/tn-assembly-constituencies.geojson`, from DataMeet's `assembly-constituencies/India_AC` (github.com/datameet/maps), Tamil Nadu only, simplified to ~20 m | CC BY 2.5 IN. Scraped from ECI maps; boundaries are approximate and shifted in places. Districts are as at the 2008 delimitation |

For heavy use (many people, public launch), use a commercial tile and
geocoding provider or self-host them, and change the URLs in
`components/mapController.ts` and `lib/osm.ts`.

## Default prices

The default cost sheet (`lib/costs.ts`) uses the priced PCB V1 BoM
(`docs/hardware/pcb-v1/flood-mesh-v1.0-bom-priced.xlsx`, ₹2,417 landed per
unit for a 15-unit build), the §14.1 whip antenna (₹307) and WPC type
approval (`docs/certification-india.md`, ~₹10,000 per model). Everything else
is marked "Estimate" and should be replaced with quotes on the admin page.

## How it is built

| Part | What |
|---|---|
| Framework | Next.js 16 (App Router, TypeScript), the same stack as `webapp/field-logger` |
| Map | Leaflet, Leaflet-Geoman (drawing and editing) |
| Database | Neon Postgres when `DATABASE_URL` is set (Vercel); otherwise PGlite in `./.pglite`. Tables: `settings` (the cost sheet), `login_limits` |
| Admin login | One password (`ADMIN_PASSWORD`); HMAC-signed, httpOnly session cookie (8 h); rate-limited (10 wrong passwords per IP per 15 min); same-origin check on every write |

Pages: `/` (planner), `/admin`. API: `GET /api/costs` (public prices),
`GET|PUT|DELETE /api/admin/costs` (admin), `/api/login`, `/api/logout`.

## Settings

| Variable | Required | Meaning |
|---|---|---|
| `ADMIN_PASSWORD` | yes | Password for `/admin` |
| `SESSION_SECRET` | yes | Signs the login cookie, at least 32 random characters |
| `DATABASE_URL` | on Vercel | Neon connection string; unset locally = PGlite |
| `TRUST_PROXY` | no | `1` only behind a self-hosted reverse proxy that appends the client address to `X-Forwarded-For` |

```sh
node -e "console.log(require('crypto').randomBytes(32).toString('base64url'))"   # SESSION_SECRET
node -e "console.log(require('crypto').randomBytes(12).toString('base64url'))"   # ADMIN_PASSWORD
```

## Local use

Needs Node.js 22.18 or newer.

```sh
cd webapp/coverage-planner
npm install
cp .env.example .env.local      # fill in ADMIN_PASSWORD and SESSION_SECRET
npm run dev                     # http://localhost:3000, admin at /admin
npm test                        # radio, placement and cost tests
```

For a meeting without internet the map tiles and searches will not load;
the plan, coverage and costs still work on a saved plan.

## Deploying on Vercel

As for the field logger (`webapp/field-logger/README.md`, "Deploying on
Vercel"), with **Root Directory** `webapp/coverage-planner`, the variables
above, a Neon database from the **Storage** tab, function region Mumbai
(`bom1`), and the production branch set to the one that contains this app.
A separate Vercel project and database from the field logger keeps test data
and prices apart.
