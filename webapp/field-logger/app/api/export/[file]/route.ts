/**
 * GET /api/export/<file>.csv[?unit=A]: admin-only CSV exports for the
 * post-test analysis.
 *
 *   records.csv                every stored unit record
 *   locations.csv              every shared phone position
 *   notes.csv                  every volunteer note
 *   readings_with_location.csv every rx record with the receiving unit's and
 *                              the sending unit's nearest phone position
 *                              within +/-2 min, and the distance between them
 *   conflicts.csv              records that reused a seq already stored with
 *                              different content (protocol section 4), with
 *                              the MAC of the board that sent them
 *
 * Times: ISO 8601 UTC in `ts`/`received_at`, plus `ts_local` in TIMEZONE.
 * Pages are read with keyset pagination and streamed out.
 */
import { isUnit } from "@/lib/config";
import { iso, num, query, str, type Row } from "@/lib/db";
import { csvStream, localTime } from "@/lib/csv";
import { haversineM } from "@/lib/geo";
import { json, requireSession } from "@/lib/http";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const PAGE = 2000;
// How far from a reading a phone position may be and still describe where
// the unit was (protocol section 8).
const LOCATION_WINDOW = "2 minutes";

type Rows = AsyncGenerator<unknown[]>;

/** Keyset pages until a short page. `next` builds the params after `last`. */
async function* paged(sql: string, first: unknown[], next: (last: Row) => unknown[], map: (r: Row) => unknown[]): Rows {
  let params = first;
  for (;;) {
    const rows = await query(sql, [...params, PAGE]);
    for (const r of rows) yield map(r);
    if (rows.length < PAGE) return;
    params = next(rows[rows.length - 1]);
  }
}

const RECORD_HEADER = [
  "unit", "seq", "boot", "ms", "device_epoch", "ts", "ts_local", "ts_src", "received_at", "k", "t", "from",
  "id", "hops", "rssi", "snr", "text", "ch", "ok", "reason", "fw", "image", "state", "n", "batt_v",
  "batt_pct", "powered", "air_pm", "heard", "sleep_pct", "role", "extra", "mac",
];

/** One records / record_conflicts row as a RECORD_HEADER line. */
function recordLine(r: Row): unknown[] {
  const ts = iso(r.ts);
  return [
    str(r.unit), num(r.seq), num(r.boot), num(r.ms), num(r.device_epoch), ts, localTime(ts), str(r.ts_src),
    iso(r.received_at), str(r.k), str(r.t), str(r.from_call), str(r.msg_id), num(r.hops), num(r.rssi),
    num(r.snr), str(r.text), num(r.ch), r.ok, str(r.reason), str(r.fw), str(r.image), str(r.state),
    num(r.n), num(r.batt_v), num(r.batt_pct), r.powered, num(r.air_pm), num(r.heard), num(r.sleep_pct),
    str(r.role), r.extra ?? null, str(r.mac) || null,
  ];
}

function records(unit: string | null): Rows {
  return paged(
    `SELECT * FROM records
      WHERE (unit, seq) > ($1, $2) AND ($3::text IS NULL OR unit = $3)
      ORDER BY unit, seq LIMIT $4`,
    ["", -1, unit],
    (last) => [last.unit, num(last.seq), unit],
    recordLine,
  );
}

/** Seq collisions, in the order they arrived; `mac` tells the boards apart. */
function conflicts(unit: string | null): Rows {
  return paged(
    `SELECT * FROM record_conflicts WHERE id > $1 AND ($2::text IS NULL OR unit = $2) ORDER BY id LIMIT $3`,
    [0, unit],
    (last) => [num(last.id), unit],
    recordLine,
  );
}

const LOCATION_HEADER = ["unit", "ts", "ts_local", "lat", "lon", "accuracy_m", "altitude_m", "received_at"];

function locations(unit: string | null): Rows {
  return paged(
    `SELECT * FROM locations
      WHERE (unit, ts, id) > ($1, $2::timestamptz, $3) AND ($4::text IS NULL OR unit = $4)
      ORDER BY unit, ts, id LIMIT $5`,
    ["", "1970-01-01T00:00:00Z", 0, unit],
    (last) => [last.unit, iso(last.ts), num(last.id), unit],
    (r) => {
      const ts = iso(r.ts);
      return [str(r.unit), ts, localTime(ts), num(r.lat), num(r.lon), num(r.accuracy), num(r.altitude), iso(r.received_at)];
    },
  );
}

const NOTE_HEADER = ["unit", "ts", "ts_local", "author", "text"];

function notes(unit: string | null): Rows {
  return paged(
    `SELECT * FROM notes WHERE id > $1 AND ($2::text IS NULL OR unit = $2) ORDER BY id LIMIT $3`,
    [0, unit],
    (last) => [num(last.id), unit],
    (r) => {
      const ts = iso(r.ts);
      return [str(r.unit), ts, localTime(ts), str(r.author), str(r.text)];
    },
  );
}

const READING_HEADER = [
  "unit", "seq", "ts", "ts_local", "ts_src", "t", "from", "id", "hops", "rssi", "snr", "text", "ch",
  "rx_lat", "rx_lon", "rx_accuracy_m", "rx_loc_ts", "rx_loc_dt_s",
  "tx_lat", "tx_lon", "tx_accuracy_m", "tx_loc_ts", "tx_loc_dt_s",
  "distance_m",
];

/**
 * Nearest position of one phone to the reading's time, within the window.
 * `from` is the frame's originator: for hops > 0 the frame was heard from a
 * relay, so distance_m is receiver-to-originator, not the radio link. Filter
 * on hops = 0 for link-budget analysis.
 */
const NEAREST = (who: string) => `LEFT JOIN LATERAL (
    SELECT l.lat, l.lon, l.accuracy, l.ts FROM locations l
     WHERE l.unit = ${who}
       AND l.ts BETWEEN r.ts - interval '${LOCATION_WINDOW}' AND r.ts + interval '${LOCATION_WINDOW}'
     ORDER BY abs(extract(epoch FROM l.ts - r.ts)), l.id
     LIMIT 1)`;

function readings(unit: string | null): Rows {
  return paged(
    `SELECT r.unit, r.seq, r.ts, r.ts_src, r.t, r.from_call, r.msg_id, r.hops, r.rssi, r.snr, r.text, r.ch,
            rl.lat AS rx_lat, rl.lon AS rx_lon, rl.accuracy AS rx_acc, rl.ts AS rx_ts,
            sl.lat AS tx_lat, sl.lon AS tx_lon, sl.accuracy AS tx_acc, sl.ts AS tx_ts
       FROM records r
       ${NEAREST("r.unit")} rl ON true
       ${NEAREST("r.from_call")} sl ON true
      WHERE r.k = 'rx' AND (r.unit, r.seq) > ($1, $2) AND ($3::text IS NULL OR r.unit = $3)
      ORDER BY r.unit, r.seq LIMIT $4`,
    ["", -1, unit],
    (last) => [last.unit, num(last.seq), unit],
    (r) => {
      const ts = iso(r.ts);
      const tMs = ts ? Date.parse(ts) : NaN;
      const dt = (v: unknown) => {
        const t = iso(v);
        return t && Number.isFinite(tMs) ? Math.round((Date.parse(t) - tMs) / 100) / 10 : null;
      };
      const [rxLat, rxLon, txLat, txLon] = [num(r.rx_lat), num(r.rx_lon), num(r.tx_lat), num(r.tx_lon)];
      const dist =
        rxLat !== null && rxLon !== null && txLat !== null && txLon !== null
          ? Math.round(haversineM(rxLat, rxLon, txLat, txLon) * 10) / 10
          : null;
      return [
        str(r.unit), num(r.seq), ts, localTime(ts), str(r.ts_src), str(r.t), str(r.from_call), str(r.msg_id),
        num(r.hops), num(r.rssi), num(r.snr), str(r.text), num(r.ch),
        rxLat, rxLon, num(r.rx_acc), iso(r.rx_ts), dt(r.rx_ts),
        txLat, txLon, num(r.tx_acc), iso(r.tx_ts), dt(r.tx_ts),
        dist,
      ];
    },
  );
}

type Export = { header: string[]; rows: (unit: string | null) => Rows };
const EXPORTS: Record<string, Export> = {
  "records.csv": { header: RECORD_HEADER, rows: records },
  "locations.csv": { header: LOCATION_HEADER, rows: locations },
  "notes.csv": { header: NOTE_HEADER, rows: notes },
  "readings_with_location.csv": { header: READING_HEADER, rows: readings },
  "conflicts.csv": { header: RECORD_HEADER, rows: conflicts },
};

/** Own keys only: EXPORTS["constructor"] would otherwise find Object and fail with a 500. */
function findExport(file: string): Export | undefined {
  return Object.hasOwn(EXPORTS, file) ? EXPORTS[file] : undefined;
}

export async function GET(req: Request, ctx: { params: Promise<{ file: string }> }): Promise<Response> {
  const s = await requireSession();
  if (s instanceof Response) return s;
  if (s.role !== "admin") return json({ ok: false, error: "admin only" }, 403);

  const { file } = await ctx.params;
  const exp = findExport(file);
  if (!exp) return json({ ok: false, error: "unknown export" }, 404);
  const asked = new URL(req.url).searchParams.get("unit");
  if (asked !== null && !isUnit(asked)) return json({ ok: false, error: "unknown unit" }, 400);

  const stamp = new Date().toISOString().slice(0, 16).replace(/[-:]/g, "").replace("T", "-");
  const name = `floodmesh-${asked ? `${asked}-` : ""}${file.replace(".csv", "")}-${stamp}.csv`;
  console.log(`[export] ${file}${asked ? ` unit ${asked}` : ""}`);
  return new Response(csvStream(exp.header, exp.rows(asked), file), {
    headers: {
      "Content-Type": "text/csv; charset=utf-8",
      "Content-Disposition": `attachment; filename="${name}"`,
      "Cache-Control": "no-store",
    },
  });
}
