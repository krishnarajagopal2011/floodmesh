/**
 * Unit uploads (docs/field-logger-protocol.md sections 2-5): validation,
 * record time, and the idempotent insert.
 *
 * Validation is strict only on what the server relies on: the envelope and
 * the five common record fields (seq, boot, ms, epoch, k), which give the
 * key and the time rule. Those answer 400 with the exact path of the first
 * problem. Everything else is kept rather than refused: unknown kinds and
 * unknown fields go to `extra`, as the protocol requires, and so does a
 * known field whose value has the wrong JSON type. A 400 makes the unit
 * resend the same oldest-first batch every window, so one odd field from a
 * newer firmware would otherwise block that unit's whole log until someone
 * ran `log clear`. Type mismatches are reported back to the caller for the
 * server log, so firmware bugs still show up at the bench.
 */
import type { Db } from "./db";

export const MAX_BODY_BYTES = 16 * 1024;
export const MAX_RECORDS = 100;
export const PROTOCOL_VERSION = 1;

// uint32 seconds reach 2106; anything larger is not a clock reading.
const MAX_EPOCH = 0xffffffff;
const INT32_MIN = -0x80000000;
const INT32_MAX = 0x7fffffff;

export class BadRequest extends Error {}

export interface Envelope {
  v: number;
  unit: string;
  mac: string | null;
  fw: string | null;
  image: string | null;
  boot: number;
  up_ms: number;
  epoch: number;
  dropped: number | null;
}

/** One row for the `records` table, keys = column names. */
export interface RecordRow {
  unit: string;
  seq: number;
  boot: number;
  ms: number;
  device_epoch: number;
  ts: string | null;
  ts_src: "unit" | "uptime" | null;
  received_at: string;
  k: string;
  t: string | null;
  from_call: string | null;
  msg_id: string | null;
  hops: number | null;
  rssi: number | null;
  snr: number | null;
  text: string | null;
  ch: number | null;
  ok: boolean | null;
  reason: string | null;
  fw: string | null;
  image: string | null;
  state: string | null;
  n: number | null;
  batt_v: number | null;
  batt_pct: number | null;
  powered: boolean | null;
  air_pm: number | null;
  heard: number | null;
  sleep_pct: number | null;
  role: string | null;
  extra: Record<string, unknown> | null;
  /** Factory MAC from the upload's envelope: which board made the record. */
  mac: string | null;
}

type FieldType = "str" | "int" | "num" | "bool" | "id";
type Column = Exclude<
  keyof RecordRow,
  "unit" | "seq" | "boot" | "ms" | "device_epoch" | "ts" | "ts_src" | "received_at" | "k" | "extra" | "mac"
>;

/** Protocol field name -> column and JSON type (section 3). */
const FIELDS: Record<string, { col: Column; type: FieldType; max?: number }> = {
  t: { col: "t", type: "str", max: 32 },
  from: { col: "from_call", type: "str", max: 32 },
  id: { col: "msg_id", type: "id" },
  hops: { col: "hops", type: "int" },
  rssi: { col: "rssi", type: "num" },
  snr: { col: "snr", type: "num" },
  // The firmware caps texts at 60 characters; the server allows slack so a
  // longer one is stored rather than blocking the unit's whole log.
  text: { col: "text", type: "str", max: 500 },
  ch: { col: "ch", type: "int" },
  ok: { col: "ok", type: "bool" },
  reason: { col: "reason", type: "str", max: 64 },
  fw: { col: "fw", type: "str", max: 32 },
  image: { col: "image", type: "str", max: 64 },
  state: { col: "state", type: "str", max: 32 },
  n: { col: "n", type: "int" },
  batt_v: { col: "batt_v", type: "num" },
  batt_pct: { col: "batt_pct", type: "int" },
  powered: { col: "powered", type: "bool" },
  air_pm: { col: "air_pm", type: "int" },
  heard: { col: "heard", type: "int" },
  sleep_pct: { col: "sleep_pct", type: "int" },
  role: { col: "role", type: "str", max: 32 },
};
const COMMON = new Set(["seq", "boot", "ms", "epoch", "k"]);
const TYPE_NAMES: Record<FieldType, string> = {
  str: "string",
  int: "int",
  num: "number",
  bool: "bool",
  id: "int or short string",
};

/**
 * FIELDS is a plain object, so FIELDS["constructor"] or FIELDS["toString"]
 * would return what it inherits from Object.prototype and an unknown field of
 * that name would be taken for a known one and lost. Own keys only.
 */
function fieldSpec(key: string): (typeof FIELDS)[string] | undefined {
  return Object.hasOwn(FIELDS, key) ? FIELDS[key] : undefined;
}

function isObj(v: unknown): v is Record<string, unknown> {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}

/** Postgres text and jsonb cannot hold U+0000; a stray NUL would make every retry fail. */
function clean(s: string): string {
  return s.includes("\u0000") ? s.replaceAll("\u0000", "") : s;
}

/**
 * An object with no prototype: a key such as "__proto__" is then stored as an
 * ordinary property (a plain {} would run the inherited setter and drop it),
 * and JSON.stringify still writes it out.
 */
function bag(): Record<string, unknown> {
  return Object.create(null) as Record<string, unknown>;
}

function cleanDeep(v: unknown): unknown {
  if (typeof v === "string") return clean(v);
  if (Array.isArray(v)) return v.map(cleanDeep);
  if (isObj(v)) {
    const out = bag();
    for (const [key, val] of Object.entries(v)) out[clean(key)] = cleanDeep(val);
    return out;
  }
  return v;
}

function uint(v: unknown, path: string, max = Number.MAX_SAFE_INTEGER): number {
  if (typeof v !== "number" || !Number.isSafeInteger(v) || v < 0 || v > max) {
    throw new BadRequest(`${path}: expected a non-negative integer`);
  }
  return v;
}

function optStr(v: unknown, path: string, max: number): string | null {
  if (v === undefined || v === null) return null;
  if (typeof v !== "string") throw new BadRequest(`${path}: expected a string`);
  if (v.length > max) throw new BadRequest(`${path}: longer than ${max} characters`);
  return clean(v);
}

/**
 * A protocol field's value in its column's type, or undefined when the JSON
 * type does not match (the caller then keeps it in `extra`).
 *
 * Strings are stored exactly as sent apart from NUL. The unit already trims
 * call signs, and `text` must match what it sent and what RXLOG printed, so
 * a leading or trailing space in a text is data, not padding.
 */
function typed(v: unknown, type: FieldType, max = 64): string | number | boolean | undefined {
  switch (type) {
    case "str":
      return typeof v === "string" && v.length <= max ? clean(v) : undefined;
    case "int":
      return typeof v === "number" && Number.isInteger(v) && v >= INT32_MIN && v <= INT32_MAX ? v : undefined;
    case "num":
      return typeof v === "number" && Number.isFinite(v) ? v : undefined;
    case "bool":
      return typeof v === "boolean" ? v : undefined;
    case "id":
      // Frame ids are printed as unsigned integers today; a string id is
      // accepted too so the protocol can change representation.
      if (typeof v === "number" && Number.isSafeInteger(v)) return String(v);
      if (typeof v === "string" && v.length <= 32) return clean(v);
      return undefined;
  }
}

/** Validate the parsed body; throws BadRequest. Records are returned un-timed. */
export function parseBody(body: unknown): { env: Envelope; records: Record<string, unknown>[] } {
  if (!isObj(body)) throw new BadRequest("body: expected a JSON object");
  if (body.v !== PROTOCOL_VERSION) throw new BadRequest(`v: expected ${PROTOCOL_VERSION}`);
  if (typeof body.unit !== "string" || body.unit.length === 0 || body.unit.length > 16) {
    throw new BadRequest("unit: expected a call sign");
  }
  const env: Envelope = {
    v: PROTOCOL_VERSION,
    unit: body.unit,
    mac: optStr(body.mac, "mac", 32),
    fw: optStr(body.fw, "fw", 32),
    image: optStr(body.image, "image", 64),
    boot: uint(body.boot, "boot"),
    up_ms: uint(body.up_ms, "up_ms"),
    epoch: uint(body.epoch, "epoch", MAX_EPOCH),
    dropped: body.dropped === undefined || body.dropped === null ? null : uint(body.dropped, "dropped"),
  };
  if (!Array.isArray(body.records)) throw new BadRequest("records: expected an array");
  if (body.records.length > MAX_RECORDS) throw new BadRequest(`records: more than ${MAX_RECORDS}`);
  body.records.forEach((r, i) => {
    if (!isObj(r)) throw new BadRequest(`records[${i}]: expected an object`);
  });
  return { env, records: body.records as Record<string, unknown>[] };
}

/**
 * Turn one validated-envelope record into a table row.
 *
 * Time (protocol section 5): the unit's own epoch when its clock was set;
 * otherwise, for a record made during the boot that sent this request,
 * receive time minus the uptime elapsed since the record (the two uptimes
 * share a zero); otherwise unknown. `seq` still orders unknown-time records.
 *
 * A known field with the wrong JSON type is kept in `extra` under its own
 * name and listed in `extra._type_errors`; a line for the server log is
 * pushed to `problems`.
 */
export function toRow(
  rec: Record<string, unknown>,
  i: number,
  env: Envelope,
  receivedAtMs: number,
  problems: string[] = [],
): RecordRow {
  const p = `records[${i}]`;
  const seq = uint(rec.seq, `${p}.seq`);
  const boot = uint(rec.boot, `${p}.boot`);
  const ms = uint(rec.ms, `${p}.ms`);
  const epoch = uint(rec.epoch, `${p}.epoch`, MAX_EPOCH);
  if (typeof rec.k !== "string" || rec.k.length === 0 || rec.k.length > 16) {
    throw new BadRequest(`${p}.k: expected a record kind`);
  }

  let ts: string | null = null;
  let tsSrc: RecordRow["ts_src"] = null;
  if (epoch > 0) {
    ts = new Date(epoch * 1000).toISOString();
    tsSrc = "unit";
  } else if (boot === env.boot && ms <= env.up_ms) {
    ts = new Date(receivedAtMs - (env.up_ms - ms)).toISOString();
    tsSrc = "uptime";
  }

  const row: RecordRow = {
    unit: env.unit,
    seq,
    boot,
    ms,
    device_epoch: epoch,
    ts,
    ts_src: tsSrc,
    received_at: new Date(receivedAtMs).toISOString(),
    k: clean(rec.k),
    t: null, from_call: null, msg_id: null, hops: null, rssi: null, snr: null, text: null, ch: null,
    ok: null, reason: null, fw: null, image: null, state: null, n: null, batt_v: null, batt_pct: null,
    powered: null, air_pm: null, heard: null, sleep_pct: null, role: null,
    extra: null,
    mac: env.mac,
  };

  const extra = bag();
  const typeErrors: string[] = [];
  for (const [key, value] of Object.entries(rec)) {
    if (COMMON.has(key)) continue;
    const spec = fieldSpec(key);
    if (!spec) {
      extra[clean(key)] = cleanDeep(value);
      continue;
    }
    if (value === null || value === undefined) continue; // null = not applicable
    const v = typed(value, spec.type, spec.max);
    if (v === undefined) {
      extra[key] = cleanDeep(value);
      typeErrors.push(key);
      problems.push(`seq ${seq} field ${key} kept in extra (expected ${TYPE_NAMES[spec.type]})`);
      continue;
    }
    (row as unknown as Record<string, unknown>)[spec.col] = v;
  }
  if (typeErrors.length > 0) extra._type_errors = typeErrors;
  if (Object.keys(extra).length > 0) row.extra = extra;
  return row;
}

export interface StoreResult {
  /** Rows new in `records`. */
  inserted: number;
  /** Genuine re-sends: identical to the row already stored under that seq. */
  duplicates: number;
  /** Seq collisions: a different record under a seq already stored (see storeUpload). */
  conflicts: number;
  /**
   * For the server log: first and last colliding seq, what the stored row
   * under the first one says, and how many were new to record_conflicts
   * (the rest had been kept by an earlier attempt).
   */
  conflict?: { firstSeq: number; lastSeq: number; storedBoot: number | null; storedMac: string | null; newlyKept: number };
  ackedSeq: number | null;
}

const RECORD_COLUMNS = `unit, seq, boot, ms, device_epoch, ts, ts_src, received_at, k, t, from_call, msg_id,
  hops, rssi, snr, text, ch, ok, reason, fw, image, state, n, batt_v, batt_pct, powered, air_pm, heard,
  sleep_pct, role, extra, mac`;

/**
 * Everything the unit sent for a record. A re-send from the unit's flash
 * repeats all of it exactly; ts and received_at are the server's and differ.
 */
const SENT_FIELDS = `boot, ms, device_epoch, k, t, from_call, msg_id, hops, rssi, snr, text, ch, ok, reason, fw,
  image, state, n, batt_v, batt_pct, powered, air_pm, heard, sleep_pct, role, extra`;
const sentOf = (alias: string) => `(${SENT_FIELDS.split(",").map((c) => `${alias}.${c.trim()}`).join(", ")})`;

/**
 * Store the records and the upload row in one statement, so all of it lands
 * or none does. The records travel as one JSON parameter expanded by
 * jsonb_to_recordset: one round trip on Neon, same SQL on PGlite.
 *
 * Each record whose (unit, seq) is already stored is compared with the
 * stored row (protocol section 4):
 *   - same content (and same board when both MACs are known): a re-send,
 *     counted as a duplicate;
 *   - different: a seq collision. seq restarts at 1 when a board's NVS and
 *     flash are erased, or when a spare board is given a unit's call sign,
 *     and those records must not be mistaken for re-sends. They go to
 *     `record_conflicts` with the board's MAC instead of being dropped.
 * Content, not just (boot, ms, k), decides: an erased board replays its
 * first boot with the same boot counter and very nearly the same timing.
 *
 * Every record of the request is then stored somewhere, so the highest seq
 * is acknowledged, collisions included. Refusing them instead would make the
 * unit resend the same oldest-first batch every window and stall its whole
 * log; the collision is made visible in the server log and on the admin
 * page instead.
 *
 * The classification reads one snapshot. If a row was neither inserted nor
 * found (two requests for the same unit racing, or one request carrying the
 * same seq twice), nothing is acknowledged: the error becomes a 500, the
 * unit retries next window and the retry sees the committed rows.
 */
export async function storeUpload(
  db: Db,
  env: Envelope,
  rows: RecordRow[],
  receivedAtMs: number,
  bodyBytes: number,
): Promise<StoreResult> {
  const seqs = rows.map((r) => r.seq);
  const minSeq = seqs.length ? Math.min(...seqs) : null;
  const maxSeq = seqs.length ? Math.max(...seqs) : null;
  const result = await db.query<Record<string, unknown>>(
    `WITH incoming AS (
       SELECT * FROM jsonb_to_recordset(($1::text)::jsonb) AS r(
         unit text, seq bigint, boot bigint, ms bigint, device_epoch bigint, ts timestamptz, ts_src text,
         received_at timestamptz, k text, t text, from_call text, msg_id text, hops integer,
         rssi double precision, snr double precision, text text, ch integer, ok boolean, reason text,
         fw text, image text, state text, n integer, batt_v double precision, batt_pct integer,
         powered boolean, air_pm integer, heard integer, sleep_pct integer, role text, extra jsonb, mac text,
         idx integer)
     ), existing AS (
       SELECT i.idx, i.seq, s.boot AS stored_boot, s.mac AS stored_mac,
              (${sentOf("i")} IS NOT DISTINCT FROM ${sentOf("s")}
               AND (i.mac IS NULL OR s.mac IS NULL OR i.mac = s.mac)) AS same
         FROM incoming i JOIN records s ON s.unit = i.unit AND s.seq = i.seq
     ), ins AS (
       INSERT INTO records (${RECORD_COLUMNS})
       SELECT ${RECORD_COLUMNS} FROM incoming i
        WHERE NOT EXISTS (SELECT 1 FROM existing e WHERE e.idx = i.idx)
       ON CONFLICT (unit, seq) DO NOTHING
       RETURNING 1
     ), conf AS (
       INSERT INTO record_conflicts (${RECORD_COLUMNS})
       SELECT ${RECORD_COLUMNS.replace("extra, mac", "extra, coalesce(i.mac, '')")} FROM incoming i
        WHERE EXISTS (SELECT 1 FROM existing e WHERE e.idx = i.idx AND NOT e.same)
       ON CONFLICT (unit, seq, boot, ms, k, mac) DO NOTHING
       RETURNING 1
     ), up AS (
       INSERT INTO uploads (unit, received_at, v, mac, fw, image, boot, up_ms, device_epoch, dropped,
                            n_records, n_new, n_conflicts, min_seq, max_seq, body_bytes)
       VALUES ($2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, (SELECT count(*) FROM ins),
               (SELECT count(*) FROM existing WHERE NOT same), $13, $14, $15)
       RETURNING n_new
     )
     SELECT (SELECT n_new FROM up)::int AS n_new,
            (SELECT count(*) FROM existing WHERE same)::int AS n_same,
            (SELECT count(*) FROM existing WHERE NOT same)::int AS n_conflict,
            (SELECT count(*) FROM conf)::int AS n_conflict_new,
            c.first_seq, c.last_seq, c.stored_boot, c.stored_mac
       FROM (SELECT min(seq) AS first_seq, max(seq) AS last_seq,
                    (array_agg(stored_boot ORDER BY seq))[1] AS stored_boot,
                    (array_agg(stored_mac ORDER BY seq))[1] AS stored_mac
               FROM existing WHERE NOT same) c`,
    [
      // idx = position in the request, so each row is classified on its own
      // even if a request carries one seq twice.
      JSON.stringify(rows.map((row, idx) => ({ ...row, idx }))),
      env.unit,
      new Date(receivedAtMs).toISOString(),
      env.v,
      env.mac,
      env.fw,
      env.image,
      env.boot,
      env.up_ms,
      env.epoch,
      env.dropped,
      rows.length,
      minSeq,
      maxSeq,
      bodyBytes,
    ],
  );
  const r = result[0] ?? {};
  const inserted = Number(r.n_new ?? 0);
  const duplicates = Number(r.n_same ?? 0);
  const conflicts = Number(r.n_conflict ?? 0);
  if (inserted + duplicates + conflicts !== rows.length) {
    throw new Error(
      `${rows.length - inserted - duplicates - conflicts} of ${rows.length} records neither stored nor found ` +
        `(same seq twice, or a concurrent upload); not acknowledged, the unit will resend`,
    );
  }
  return {
    inserted,
    duplicates,
    conflicts,
    conflict:
      conflicts > 0
        ? {
            firstSeq: Number(r.first_seq),
            lastSeq: Number(r.last_seq),
            storedBoot: r.stored_boot === null || r.stored_boot === undefined ? null : Number(r.stored_boot),
            storedMac: r.stored_mac === null || r.stored_mac === undefined ? null : String(r.stored_mac),
            newlyKept: Number(r.n_conflict_new ?? 0),
          }
        : undefined,
    // Every record of the request is now stored (new, already present, or
    // kept as a collision), so the unit may delete up to the highest seq.
    ackedSeq: maxSeq,
  };
}
