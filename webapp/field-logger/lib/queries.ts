/**
 * Read queries behind the volunteer and admin pages. Results are normalised
 * (numbers as numbers, times as ISO strings) because Neon and PGlite return
 * bigint and timestamps in different shapes.
 */
import { iso, num, query, str, type Row } from "./db";
import { timeZone, units } from "./config";

export interface RecordView {
  seq: number;
  k: string;
  t: string | null;
  ts: string | null;
  ts_src: string | null;
  received_at: string | null;
  from: string | null;
  id: string | null;
  hops: number | null;
  rssi: number | null;
  snr: number | null;
  text: string | null;
  ch: number | null;
  ok: boolean | null;
  state: string | null;
  n: number | null;
  reason: string | null;
  batt_v: number | null;
  batt_pct: number | null;
  powered: boolean | null;
}

export interface UploadView {
  received_at: string | null;
  fw: string | null;
  image: string | null;
  mac: string | null;
  boot: number | null;
  dropped: number | null;
  n_records: number | null;
  n_new: number | null;
}

export interface BatteryView {
  batt_v: number | null;
  batt_pct: number | null;
  powered: boolean | null;
  air_pm: number | null;
  heard: number | null;
  sleep_pct: number | null;
  role: string | null;
  at: string | null;
}

export interface LocationView {
  ts: string | null;
  lat: number | null;
  lon: number | null;
  accuracy: number | null;
}

export interface NoteView {
  ts: string | null;
  author: string | null;
  text: string | null;
}

/**
 * Signs that two boards share this call sign, or one board's seq restarted
 * (protocol section 4). `boards` lists every MAC that has uploaded as the
 * unit; `conflicts` counts records kept apart because their seq was already
 * stored with different content.
 */
export interface BoardCheck {
  boards: string[];
  conflicts: number;
}

export interface UnitStatus extends BoardCheck {
  unit: string;
  server_time: string;
  upload: UploadView | null;
  battery: BatteryView | null;
  records: RecordView[];
  totals: { all: number; today: number };
  location: LocationView | null;
  notes: NoteView[];
}

const BOARDS = (unit: string) =>
  `(SELECT string_agg(DISTINCT mac, ' ' ORDER BY mac) FROM uploads WHERE unit = ${unit} AND mac <> '')`;
const CONFLICTS = (unit: string) => `(SELECT count(*) FROM record_conflicts WHERE unit = ${unit})::int`;

function boardList(v: unknown): string[] {
  return typeof v === "string" && v ? v.split(" ") : [];
}

function bool(v: unknown): boolean | null {
  return v === null || v === undefined ? null : Boolean(v);
}

function recordView(r: Row): RecordView {
  return {
    seq: num(r.seq) ?? 0,
    k: String(r.k),
    t: str(r.t),
    ts: iso(r.ts),
    ts_src: str(r.ts_src),
    received_at: iso(r.received_at),
    from: str(r.from_call),
    id: str(r.msg_id),
    hops: num(r.hops),
    rssi: num(r.rssi),
    snr: num(r.snr),
    text: str(r.text),
    ch: num(r.ch),
    ok: bool(r.ok),
    state: str(r.state),
    n: num(r.n),
    reason: str(r.reason),
    batt_v: num(r.batt_v),
    batt_pct: num(r.batt_pct),
    powered: bool(r.powered),
  };
}

function uploadView(r: Row | undefined): UploadView | null {
  if (!r) return null;
  return {
    received_at: iso(r.received_at),
    fw: str(r.fw),
    image: str(r.image),
    mac: str(r.mac),
    boot: num(r.boot),
    dropped: num(r.dropped),
    n_records: num(r.n_records),
    n_new: num(r.n_new),
  };
}

function batteryView(r: Row | undefined): BatteryView | null {
  if (!r) return null;
  return {
    batt_v: num(r.batt_v),
    batt_pct: num(r.batt_pct),
    powered: bool(r.powered),
    air_pm: num(r.air_pm),
    heard: num(r.heard),
    sleep_pct: num(r.sleep_pct),
    role: str(r.role),
    at: iso(r.ts) ?? iso(r.received_at),
  };
}

/** Midnight today in the test's time zone, as a Postgres expression. */
const TODAY = `(date_trunc('day', now() AT TIME ZONE $TZ) AT TIME ZONE $TZ)`;

export async function unitStatus(unit: string): Promise<UnitStatus> {
  const today = TODAY.replaceAll("$TZ", "$2");
  const [uploads, battery, records, totals, location, notes, check] = await Promise.all([
    query(
      `SELECT received_at, fw, image, mac, boot, dropped, n_records, n_new
         FROM uploads WHERE unit = $1 ORDER BY received_at DESC LIMIT 1`,
      [unit],
    ),
    query(
      `SELECT batt_v, batt_pct, powered, air_pm, heard, sleep_pct, role, ts, received_at
         FROM records WHERE unit = $1 AND k = 'status' ORDER BY seq DESC LIMIT 1`,
      [unit],
    ),
    query(
      `SELECT seq, k, t, ts, ts_src, received_at, from_call, msg_id, hops, rssi, snr, text, ch, ok,
              state, n, reason, batt_v, batt_pct, powered
         FROM records WHERE unit = $1 ORDER BY seq DESC LIMIT 30`,
      [unit],
    ),
    query(
      `SELECT count(*)::int AS all_n,
              (count(*) FILTER (WHERE coalesce(ts, received_at) >= ${today}))::int AS today_n
         FROM records WHERE unit = $1`,
      [unit, timeZone()],
    ),
    query(`SELECT ts, lat, lon, accuracy FROM locations WHERE unit = $1 ORDER BY ts DESC LIMIT 1`, [unit]),
    query(`SELECT ts, author, text FROM notes WHERE unit = $1 ORDER BY ts DESC LIMIT 5`, [unit]),
    query(`SELECT ${BOARDS("$1")} AS macs, ${CONFLICTS("$1")} AS conflicts`, [unit]),
  ]);
  const loc = location[0];
  return {
    unit,
    server_time: new Date().toISOString(),
    upload: uploadView(uploads[0]),
    battery: batteryView(battery[0]),
    records: records.map(recordView),
    totals: { all: num(totals[0]?.all_n) ?? 0, today: num(totals[0]?.today_n) ?? 0 },
    location: loc ? { ts: iso(loc.ts), lat: num(loc.lat), lon: num(loc.lon), accuracy: num(loc.accuracy) } : null,
    notes: notes.map((n) => ({ ts: iso(n.ts), author: str(n.author), text: str(n.text) })),
    boards: boardList(check[0]?.macs),
    conflicts: num(check[0]?.conflicts) ?? 0,
  };
}

export interface UnitOverview extends BoardCheck {
  unit: string;
  upload: UploadView | null;
  battery: BatteryView | null;
  records_today: number;
  records_all: number;
  location: LocationView | null;
}

/** Every unit in UNITS at a glance, one query. */
export async function overview(): Promise<{ server_time: string; units: UnitOverview[] }> {
  const today = TODAY.replaceAll("$TZ", "$2");
  const rows = await query(
    `SELECT u.unit,
            up.received_at AS up_received_at, up.fw, up.image, up.mac, up.boot, up.dropped,
            up.n_records, up.n_new,
            st.batt_v, st.batt_pct, st.powered, st.air_pm, st.heard, st.sleep_pct, st.role,
            st.ts AS st_ts, st.received_at AS st_received_at,
            (SELECT count(*) FROM records r WHERE r.unit = u.unit
                AND coalesce(r.ts, r.received_at) >= ${today})::int AS records_today,
            (SELECT count(*) FROM records r WHERE r.unit = u.unit)::int AS records_all,
            loc.ts AS loc_ts, loc.lat, loc.lon, loc.accuracy,
            ${BOARDS("u.unit")} AS macs, ${CONFLICTS("u.unit")} AS conflicts
       FROM unnest(string_to_array($1, ',')) WITH ORDINALITY AS u(unit, ord)
       LEFT JOIN LATERAL (SELECT * FROM uploads WHERE unit = u.unit
                          ORDER BY received_at DESC LIMIT 1) up ON true
       LEFT JOIN LATERAL (SELECT * FROM records WHERE unit = u.unit AND k = 'status'
                          ORDER BY seq DESC LIMIT 1) st ON true
       LEFT JOIN LATERAL (SELECT * FROM locations WHERE unit = u.unit
                          ORDER BY ts DESC LIMIT 1) loc ON true
      ORDER BY u.ord`,
    [units().join(","), timeZone()],
  );
  return {
    server_time: new Date().toISOString(),
    units: rows.map((r) => ({
      unit: String(r.unit),
      upload: r.up_received_at ? uploadView({ ...r, received_at: r.up_received_at }) : null,
      battery:
        r.st_received_at ? batteryView({ ...r, ts: r.st_ts, received_at: r.st_received_at }) : null,
      records_today: num(r.records_today) ?? 0,
      records_all: num(r.records_all) ?? 0,
      location: r.loc_ts
        ? { ts: iso(r.loc_ts), lat: num(r.lat), lon: num(r.lon), accuracy: num(r.accuracy) }
        : null,
      boards: boardList(r.macs),
      conflicts: num(r.conflicts) ?? 0,
    })),
  };
}
