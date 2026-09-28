#!/usr/bin/env node
/**
 * npm run simulate -- --url <base> --key <hex> [options]
 *
 * Plays five FloodMesh units (A-E) uploading a field test's worth of logs to
 * the field-logger web app exactly as docs/field-logger-protocol.md says,
 * then checks the server's answers:
 *
 *   - every batch: 200, acked_seq = highest seq sent, sane server_epoch
 *   - re-sending a batch stores nothing twice; a half-overlapping batch
 *     stores only the new half
 *   - a different record under a seq already stored (board swapped, flash
 *     erased) is acked but kept apart as a seq collision, not dropped
 *   - a known field with the wrong type, and fields named like
 *     Object.prototype members, are stored in `extra`, not refused
 *   - bad signature, unknown unit, header/body unit mismatch: 401
 *   - body over 16 KB: 413; malformed JSON or common field: 400
 *
 * With --passcodes '{"A":"..."}' it also logs in as each unit and shares a
 * phone position; with --admin <passcode> it then downloads the CSVs and
 * checks record counts (no duplicates), the section-5 time rules, that
 * readings_with_location has distances, and that the collisions and odd
 * fields above were kept.
 *
 * Options:
 *   --url <base>        web app base URL, e.g. http://localhost:3000
 *   --key <hex>         fleet log key (LOG_KEY), 32-64 hex characters
 *   --units A,B,C,D,E   units to play
 *   --passcodes <json>  unit passcodes, enables the login/location checks
 *   --admin <passcode>  admin passcode, enables the CSV checks
 *   --env <file>        read all of the above from a dotenv file (.env.local)
 *
 * Every request is signed over exactly the bytes sent. Exit code 1 if any
 * check fails. Needs Node 18+ (global fetch), no dependencies.
 */
import { createHmac } from "node:crypto";

// ---------------------------------------------------------------- arguments
const args = {};
for (let i = 2; i < process.argv.length; i++) {
  const a = process.argv[i];
  if (a.startsWith("--")) {
    const [k, inline] = a.slice(2).split("=", 2);
    args[k] = inline ?? process.argv[++i];
  }
}
// --env <file> reads LOG_KEY, UNIT_PASSCODES, ADMIN_PASSCODE and UNITS from a
// dotenv file (e.g. .env.local) and turns on the login and CSV checks.
let fromEnv = {};
if (args.env) {
  process.loadEnvFile(args.env);
  fromEnv = {
    key: process.env.LOG_KEY,
    passcodes: process.env.UNIT_PASSCODES,
    admin: process.env.ADMIN_PASSCODE,
    units: process.env.UNITS,
  };
}
const base = (args.url ?? "").replace(/\/+$/, "");
const keyHex = (args.key ?? fromEnv.key ?? "").trim();
if (!base || !/^(?:[0-9a-fA-F]{2}){16,32}$/.test(keyHex)) {
  console.error(
    "usage: npm run simulate -- --url <base> --key <32-64 hex chars> [--units A,B,C,D,E] [--passcodes JSON] [--admin PASS]\n" +
      "   or: npm run simulate -- --url <base> --env .env.local",
  );
  process.exit(2);
}
const UNITS = (args.units ?? fromEnv.units ?? "A,B,C,D,E").split(",").map((s) => s.trim()).filter(Boolean);

/**
 * Passcodes as JSON, or as A:pass,B:pass. The second form is accepted because
 * Windows' cmd strips the double quotes out of JSON passed through npm.
 */
function parsePasscodes(text) {
  if (!text) return null;
  try {
    return JSON.parse(text);
  } catch {
    const out = {};
    for (const part of text.replace(/^\{|\}$/g, "").split(",")) {
      const m = part.match(/^\s*"?([^":=]+)"?\s*[:=]\s*"?([^"]*)"?\s*$/);
      if (m) out[m[1]] = m[2];
    }
    return Object.keys(out).length ? out : null;
  }
}
const passcodes = parsePasscodes(args.passcodes ?? fromEnv.passcodes);
const adminPass = args.admin ?? fromEnv.admin ?? null;

const MAX_BODY = 16 * 1024;
const MAX_RECORDS = 100;
const key = Buffer.from(keyHex, "hex");
const sign = (bytes) => createHmac("sha256", key).update(bytes).digest("hex");

// ---------------------------------------------------------------- results
const results = [];
function check(name, ok, detail = "") {
  results.push({ name, ok });
  console.log(`${ok ? "PASS" : "FAIL"}  ${name}${detail ? `  (${detail})` : ""}`);
  return ok;
}

// ---------------------------------------------------------------- random, repeatable per run
let rngState = Date.now() % 2147483647 || 1;
const rnd = () => (rngState = (rngState * 48271) % 2147483647) / 2147483647;
const pick = (xs) => xs[Math.floor(rnd() * xs.length)];
const between = (lo, hi) => lo + rnd() * (hi - lo);

// ---------------------------------------------------------------- the simulated fleet
// Units booted 20 minutes ago; their clocks were set by SNTP 90 s after boot,
// so the first records carry epoch 0 and get their time from uptime.
const now = Date.now();
const BOOT_AGO_MS = 20 * 60_000;
const CLOCK_SET_MS = 90_000;
const T0 = now - BOOT_AGO_MS;
// Sequence numbers start from the run's start time so repeated runs against
// the same database never collide and "inserted" counts stay exact.
const SEQ_BASE = Math.floor(now / 1000) * 1000;
const FW = "4.2.0";
const IMAGE = "2578431a";
const FOREIGN = "KXQ2R"; // a call sign that is not one of the test units
const TEXTS = ["WATER RISING", "NEED BOAT", "ALL OK HERE", "tanni varudhu", "power illa", "HELLO"];

function makeUnit(unit, index) {
  const u = {
    unit,
    mac: `30:ED:A0:CB:2F:${(0x20 + index).toString(16).toUpperCase().padStart(2, "0")}`,
    boot: 17 + index,
    seq: SEQ_BASE,
    records: [],
    msgId: 1000 * (index + 1),
  };
  const add = (ms, k, fields) => {
    const clockSet = ms >= CLOCK_SET_MS;
    u.records.push({
      seq: u.seq++,
      boot: u.boot,
      ms,
      epoch: clockSet ? Math.floor((T0 + ms) / 1000) : 0,
      k,
      ...fields,
    });
  };
  // A record left over from the previous boot, never uploaded: its time is
  // unknown to the server (protocol section 5).
  u.records.push({ seq: u.seq++, boot: u.boot - 1, ms: 3_512_000, epoch: 0, k: "status",
    batt_v: 3.71, batt_pct: 52, powered: false, air_pm: 3, heard: 2, sleep_pct: 81, role: "civilian" });
  add(800, "boot", { reason: "POWERON", fw: FW, image: IMAGE });
  add(1500, "status", { batt_v: 4.05, batt_pct: 91, powered: false, air_pm: 0, heard: 0, sleep_pct: 0, role: "civilian" });

  const others = UNITS.filter((x) => x !== unit);
  // One unknown kind and one unknown field on every unit: the server must
  // keep both (in `extra`) rather than refuse or drop them.
  add(5_000, "cad", { busy_pm: 7 });
  add(6_000, "rx", { t: "PING", from: others[0] ?? FOREIGN, id: 4242, hops: 0, rssi: -101, snr: -3.25, freq_err: -812 });
  for (let ms = 12_000; ms < BOOT_AGO_MS - 5_000; ms += Math.round(between(8_000, 22_000))) {
    const r = rnd();
    if (r < 0.62) {
      const from = rnd() < 0.1 ? FOREIGN : pick(others);
      const t = pick(["TEXT", "PING", "HEARTBEAT", "HEARTBEAT", "ACK"]);
      const hops = rnd() < 0.7 ? 0 : 1 + Math.floor(rnd() * 2);
      const rec = {
        t, from, id: 5000 + Math.floor(rnd() * 60000), hops,
        rssi: Math.round(between(-126, -68)), snr: Math.round(between(-14, 9.5) * 4) / 4,
      };
      if (t === "TEXT") rec.text = pick(TEXTS);
      if (rnd() < 0.08) rec.freq_err = Math.round(between(-2500, 2500)); // unknown field -> extra
      add(ms, "rx", rec);
      if (hops === 0 && rnd() < 0.25) add(ms + 180, "relay", { t, from, id: rec.id });
    } else if (r < 0.8) {
      const t = pick(["TEXT", "PING", "HEARTBEAT"]);
      const id = u.msgId++;
      const rec = { t, id, ok: rnd() > 0.05 };
      if (t === "TEXT") rec.text = `${pick(TEXTS)} ${unit}`;
      add(ms, "tx", rec);
      if (rnd() < 0.6) add(ms + 1200, "echo", { t, id, rssi: Math.round(between(-110, -70)), snr: Math.round(between(-6, 9) * 4) / 4 });
    } else if (r < 0.85) {
      add(ms, "cad", { busy_pm: Math.floor(between(0, 40)) }); // an unknown kind -> kept
    } else {
      add(ms, "status", {
        batt_v: Math.round(between(3.7, 4.1) * 100) / 100, batt_pct: Math.floor(between(55, 90)),
        powered: rnd() < 0.2, air_pm: Math.floor(between(1, 18)), heard: others.length,
        sleep_pct: Math.floor(between(40, 90)), role: "civilian",
      });
    }
  }

  // Own SOS stories, including one before the clock was set.
  const sosId = u.msgId++;
  if (index % 3 === 0) {
    add(40_000, "tx", { t: "SOS", id: sosId, ok: true, ch: 2 });
    add(40_010, "sos", { state: "SENT", id: sosId, ch: 2, n: 0 });
    add(70_000, "sos", { state: "RETRY", id: sosId, ch: 2, n: 1 });
    add(74_000, "rx", { t: "ACK", from: others[0], id: sosId, hops: 0, rssi: -97, snr: 4.5 });
    add(74_020, "sos", { state: "DELIVERED", id: sosId, ch: 2, n: 1 });
  } else if (index % 3 === 1) {
    add(600_000, "tx", { t: "SOS", id: sosId, ok: true, ch: 0 });
    add(600_010, "sos", { state: "SENT", id: sosId, ch: 0, n: 0 });
    add(630_000, "sos", { state: "RETRY", id: sosId, ch: 0, n: 1 });
    add(660_000, "sos", { state: "RETRY", id: sosId, ch: 0, n: 2 });
    add(690_000, "sos", { state: "NOT_DELIVERED", id: sosId, ch: 0, n: 2 });
    add(700_000, "sos", { state: "STOPPED", id: sosId, ch: 0, n: 2 });
  } else {
    add(900_000, "tx", { t: "HELP", id: sosId, ok: true, ch: 4 });
    add(900_010, "sos", { state: "HELP", id: sosId, ch: 4, n: 0 });
  }
  u.records.sort((a, b) => a.seq - b.seq);
  // add() is called out of time order for the SOS stories; the firmware
  // assigns seq in the order records are made, so renumber by time within
  // this boot (the previous-boot record stays first).
  const [prev, ...cur] = u.records;
  cur.sort((a, b) => a.ms - b.ms);
  cur.forEach((r, i) => (r.seq = prev.seq + 1 + i));
  u.records = [prev, ...cur];
  return u;
}

/** Request body; up_ms is the uptime when the request is built, as on the unit. */
function envelope(u, records, upMs = Date.now() - T0) {
  return {
    v: 1, unit: u.unit, mac: u.mac, fw: FW, image: IMAGE, boot: u.boot, up_ms: upMs,
    epoch: Math.floor((T0 + upMs) / 1000), dropped: 0, records,
  };
}

/** Split into requests of at most 100 records and 16 KB, like the unit does. */
function batches(u) {
  const out = [];
  let cur = [];
  for (const r of u.records) {
    const next = [...cur, r];
    if (next.length > MAX_RECORDS || Buffer.byteLength(JSON.stringify(envelope(u, next))) > MAX_BODY) {
      out.push(cur);
      cur = [r];
    } else {
      cur = next;
    }
  }
  if (cur.length) out.push(cur);
  return out;
}

// ---------------------------------------------------------------- HTTP
async function ingest(bodyBytes, { unit, signature } = {}) {
  const res = await fetch(`${base}/api/ingest`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "X-FM-Unit": unit,
      "X-FM-Signature": signature ?? sign(bodyBytes),
    },
    body: bodyBytes,
  });
  let body = null;
  try {
    body = await res.json();
  } catch {
    /* not JSON */
  }
  return { status: res.status, body };
}

const bytesOf = (obj) => Buffer.from(JSON.stringify(obj), "utf8");

async function login(fields) {
  const res = await fetch(`${base}/api/login`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(fields),
    redirect: "manual",
  });
  const cookie = (res.headers.getSetCookie?.() ?? []).map((c) => c.split(";")[0]).join("; ");
  return { status: res.status, cookie };
}

function parseCsv(text) {
  const rows = [];
  let row = [], cell = "", q = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (q) {
      if (c === '"' && text[i + 1] === '"') { cell += '"'; i++; }
      else if (c === '"') q = false;
      else cell += c;
    } else if (c === '"') q = true;
    else if (c === ",") { row.push(cell); cell = ""; }
    else if (c === "\n") { row.push(cell.replace(/\r$/, "")); rows.push(row); row = []; cell = ""; }
    else cell += c;
  }
  if (cell || row.length) { row.push(cell); rows.push(row); }
  const [header, ...data] = rows;
  return data.map((r) => Object.fromEntries(header.map((h, i) => [h, r[i] ?? ""])));
}

// ---------------------------------------------------------------- run
async function main() {
  console.log(`[sim] ${base}, units ${UNITS.join(",")}, seq from ${SEQ_BASE}`);
  const fleet = UNITS.map(makeUnit);
  const sentBatches = new Map();
  let firstBytes = null; // unit A's first request, kept to re-send byte for byte

  for (const u of fleet) {
    const bs = batches(u);
    sentBatches.set(u.unit, bs);
    let allOk = true;
    let detail = "";
    for (const [i, recs] of bs.entries()) {
      const bytes = bytesOf(envelope(u, recs));
      firstBytes ??= bytes;
      const { status, body } = await ingest(bytes, { unit: u.unit });
      const maxSeq = Math.max(...recs.map((r) => r.seq));
      const skew = body ? Math.abs(body.server_epoch - Date.now() / 1000) : Infinity;
      const ok = status === 200 && body?.ok === true && body.acked_seq === maxSeq && skew < 300 &&
        body.inserted === recs.length && body.duplicates === 0;
      if (!ok) {
        allOk = false;
        detail = `batch ${i}: HTTP ${status} ${JSON.stringify(body)}`;
      }
    }
    const n = u.records.length;
    check(`unit ${u.unit}: ${n} records in ${bs.length} batch(es) stored and acked`, allOk,
      allOk ? `${bs.map((b) => b.length).join("+")} records` : detail);
  }

  // Re-send: exactly the same bytes again.
  const a = fleet[0];
  const first = sentBatches.get(a.unit)[0];
  {
    const { status, body } = await ingest(firstBytes, { unit: a.unit });
    check("re-sent batch (same bytes) stores no duplicates", status === 200 && body?.inserted === 0 &&
      body?.duplicates === first.length && body?.acked_seq === Math.max(...first.map((r) => r.seq)),
      `HTTP ${status} ${JSON.stringify(body)}`);
  }
  // Half-overlapping: the last 5 records again plus 3 new ones.
  const overlap = [];
  {
    const prevLast = a.records.slice(-5);
    const upMs = Date.now() - T0;
    for (let i = 0; i < 3; i++) {
      overlap.push({ seq: a.seq++, boot: a.boot, ms: upMs - 3000 + i * 1000, epoch: Math.floor((T0 + upMs - 3000 + i * 1000) / 1000),
        k: "rx", t: "HEARTBEAT", from: fleet[1]?.unit ?? FOREIGN, id: 777 + i, hops: 0, rssi: -88 - i, snr: 6.25 });
    }
    a.records.push(...overlap);
    const recs = [...prevLast, ...overlap];
    const { status, body } = await ingest(bytesOf(envelope(a, recs, upMs)), { unit: a.unit });
    check("overlapping batch stores only the new records", status === 200 && body?.inserted === 3 &&
      body?.duplicates === 5 && body?.acked_seq === overlap[2].seq, `HTTP ${status} ${JSON.stringify(body)}`);
  }
  // Seq collisions (protocol section 4). A spare board given call sign A
  // (another MAC, boot counter back at 1) sends a record under a seq that A's
  // own board already used: acked, but kept apart rather than taken for a
  // re-send and lost. Re-sending it is a no-op like any other re-send.
  const SPARE_MAC = "30:ED:A0:CB:2F:99";
  const collided = [a.records[1].seq, a.records[3].seq];
  {
    const spare = { ...a, mac: SPARE_MAC, boot: 1 };
    const rec = { seq: collided[0], boot: 1, ms: 812, epoch: 0, k: "rx", t: "PING", from: fleet[2]?.unit ?? FOREIGN,
      id: 99, hops: 0, rssi: -110, snr: -7.5 };
    const bytes = bytesOf(envelope(spare, [rec], 60_000));
    const r1 = await ingest(bytes, { unit: a.unit });
    check("seq reused by another board: acked, kept as a collision, not a duplicate",
      r1.status === 200 && r1.body?.acked_seq === rec.seq && r1.body?.inserted === 0 && r1.body?.duplicates === 0 &&
        r1.body?.conflicts === 1, `HTTP ${r1.status} ${JSON.stringify(r1.body)}`);
    const r2 = await ingest(bytes, { unit: a.unit });
    check("re-sent collision acked again", r2.status === 200 && r2.body?.acked_seq === rec.seq &&
      r2.body?.inserted === 0 && r2.body?.conflicts === 1, `HTTP ${r2.status} ${JSON.stringify(r2.body)}`);
  }
  // Same board after its flash was erased: same MAC, so only the content
  // tells a new record from a re-send. One genuine re-send beside it.
  {
    const erased = { ...a, boot: 1 };
    const resent = a.records[2];
    const rec = { seq: collided[1], boot: 1, ms: 4_000, epoch: 0, k: "status", batt_v: 3.3, batt_pct: 5,
      powered: false, air_pm: 0, heard: 0, sleep_pct: 0, role: "civilian" };
    const { status, body } = await ingest(bytesOf(envelope(erased, [resent, rec], 60_000)), { unit: a.unit });
    check("same board, seq reused with other content: collision beside a genuine re-send",
      status === 200 && body?.acked_seq === rec.seq && body?.inserted === 0 && body?.duplicates === 1 &&
        body?.conflicts === 1, `HTTP ${status} ${JSON.stringify(body)}`);
  }

  // Fields the server must keep rather than refuse (protocol section 3): a
  // known field with the wrong type, an unknown kind reusing a known name
  // with another type, and names that Object.prototype also has. A 400 here
  // would block the unit's log: it resends the same batch every window.
  const oddSeqs = {};
  {
    const mk = () => ({ seq: a.seq++, boot: a.boot, ms: Date.now() - T0 - 400, epoch: Math.floor(Date.now() / 1000) });
    const wrongType = { ...mk(), k: "rx", t: "PING", from: fleet[1]?.unit ?? FOREIGN, hops: "two", rssi: -99, snr: 1.5 };
    const gps = { ...mk(), k: "gps", n: 1.5 };
    const chAll = { ...mk(), k: "rx", ch: "ALL" };
    // JSON.parse, so "__proto__" is an own key that JSON.stringify writes out.
    const protoNames = Object.assign(JSON.parse('{"__proto__":{"x":1}}'), mk(),
      { k: "rx", constructor: 5, toString: "x", newfield: 1 });
    const recs = [wrongType, gps, chAll, protoNames];
    Object.assign(oddSeqs, { wrongType: wrongType.seq, gps: gps.seq, chAll: chAll.seq, protoNames: protoNames.seq });
    a.records.push(...recs);
    const { status, body } = await ingest(bytesOf(envelope(a, recs)), { unit: a.unit });
    check("wrong-typed and prototype-named fields stored, not refused", status === 200 && body?.inserted === recs.length,
      `HTTP ${status} ${JSON.stringify(body)}`);
  }

  // An empty request: the unit is alive, nothing to acknowledge.
  {
    const { status, body } = await ingest(bytesOf(envelope(a, [])), { unit: a.unit });
    check("empty batch accepted", status === 200 && body?.ok === true && body?.acked_seq === null,
      `HTTP ${status} ${JSON.stringify(body)}`);
  }

  // The protocol gives the key as hex without saying whether the HMAC key is
  // the decoded bytes (what this simulator signs with) or the hex text; the
  // server accepts both so either firmware reading uploads.
  {
    const rec = { seq: a.seq++, boot: a.boot, ms: Date.now() - T0 - 500, epoch: Math.floor(Date.now() / 1000), k: "status",
      batt_v: 3.9, batt_pct: 75, powered: false, air_pm: 4, heard: fleet.length - 1, sleep_pct: 70, role: "civilian" };
    a.records.push(rec);
    const bytes = bytesOf(envelope(a, [rec]));
    const asText = createHmac("sha256", Buffer.from(keyHex, "utf8")).update(bytes).digest("hex");
    const { status, body } = await ingest(bytes, { unit: a.unit, signature: asText });
    check("signature keyed with the hex text also accepted", status === 200 && body?.inserted === 1, `HTTP ${status}`);
  }

  // Refusals.
  {
    const bytes = bytesOf(envelope(a, first.slice(0, 3)));
    const bad = sign(bytes).replace(/^./, (c) => (c === "0" ? "1" : "0"));
    const r = await ingest(bytes, { unit: a.unit, signature: bad });
    check("bad signature refused with 401", r.status === 401, `HTTP ${r.status}`);
  }
  {
    const env = envelope({ ...a, unit: "Z9" }, []);
    const r = await ingest(bytesOf(env), { unit: "Z9" });
    check("unit not in UNITS refused with 401", r.status === 401, `HTTP ${r.status}`);
  }
  if (fleet.length > 1) {
    const bytes = bytesOf(envelope(fleet[1], []));
    const r = await ingest(bytes, { unit: a.unit });
    check("X-FM-Unit differing from body.unit refused with 401", r.status === 401, `HTTP ${r.status}`);
  }
  {
    const env = { ...envelope(a, first.slice(0, 2)), pad: "x".repeat(MAX_BODY) };
    const bytes = bytesOf(env);
    const r = await ingest(bytes, { unit: a.unit });
    check(`oversized body (${bytes.length} bytes) refused with 413`, r.status === 413, `HTTP ${r.status}`);
  }
  {
    const bytes = Buffer.from('{"v":1,"unit":"' + a.unit + '","records":[', "utf8");
    const r = await ingest(bytes, { unit: a.unit });
    check("malformed JSON refused with 400", r.status === 400, `HTTP ${r.status}`);
  }
  {
    const rec = { seq: "seven", boot: a.boot, ms: 1, epoch: 0, k: "rx" };
    const r = await ingest(bytesOf(envelope(a, [rec])), { unit: a.unit });
    check("malformed common field refused with 400", r.status === 400, `HTTP ${r.status} ${r.body?.error ?? ""}`);
  }

  // ------------------------------------------------ logins and locations
  if (passcodes) {
    // Phones about 150-600 m apart around Chennai; positions one minute old.
    const origin = [13.0418, 80.2341];
    for (const [i, u] of fleet.entries()) {
      const pass = passcodes[u.unit];
      if (!pass) {
        check(`unit ${u.unit}: passcode given`, false, "missing from --passcodes");
        continue;
      }
      const { status, cookie } = await login({ role: "unit", unit: u.unit, passcode: pass });
      if (!check(`unit ${u.unit}: login`, status === 200 && cookie.includes("fm_session="), `HTTP ${status}`)) continue;
      const loc = {
        lat: origin[0] + i * 0.0014, lon: origin[1] + (i % 2) * 0.0021,
        accuracy: 6 + i, altitude: 9 + i, ts: Date.now() - 60_000,
      };
      const res = await fetch(`${base}/api/location`, {
        method: "POST", headers: { "Content-Type": "application/json", Cookie: cookie }, body: JSON.stringify(loc),
      });
      check(`unit ${u.unit}: location stored`, res.ok || res.status === 429, `HTTP ${res.status}`);
      const st = await fetch(`${base}/api/status?unit=${u.unit}`, { headers: { Cookie: cookie } });
      const body = await st.json().catch(() => null);
      check(`unit ${u.unit}: status shows its latest record`, st.ok && body?.records?.[0]?.seq === u.records.at(-1).seq,
        `HTTP ${st.status}, top seq ${body?.records?.[0]?.seq}`);
      if (fleet.length > 1) {
        const other = fleet[(i + 1) % fleet.length].unit;
        const o = await fetch(`${base}/api/status?unit=${other}`, { headers: { Cookie: cookie } });
        check(`unit ${u.unit}: cannot read unit ${other}`, o.status === 403, `HTTP ${o.status}`);
      }
    }
    const wrong = await login({ role: "unit", unit: fleet[0].unit, passcode: "definitely-wrong" });
    check("wrong passcode refused", wrong.status === 401 || wrong.status === 429, `HTTP ${wrong.status}`);
  }

  // ------------------------------------------------ CSV checks as admin
  if (adminPass) {
    const { status, cookie } = await login({ role: "admin", passcode: adminPass });
    if (check("admin login", status === 200 && cookie.includes("fm_session="), `HTTP ${status}`)) {
      for (const u of fleet) {
        const res = await fetch(`${base}/api/export/records.csv?unit=${u.unit}`, { headers: { Cookie: cookie } });
        const rows = parseCsv(await res.text()).filter((r) => Number(r.seq) >= SEQ_BASE);
        const seqs = rows.map((r) => r.seq);
        const unique = new Set(seqs).size === seqs.length;
        check(`records.csv unit ${u.unit}: ${u.records.length} rows, no duplicates`,
          res.ok && rows.length === u.records.length && unique, `HTTP ${res.status}, ${rows.length} rows`);

        // Section 5: previous-boot epoch-0 record has no time; current-boot
        // epoch-0 records are timed from uptime; later ones carry the unit's.
        const prev = rows.find((r) => Number(r.seq) === u.records[0].seq);
        const boot = rows.find((r) => r.k === "boot");
        const bootErr = boot ? Math.abs(Date.parse(boot.ts) - (T0 + 800)) : Infinity;
        const late = rows.find((r) => r.ts_src === "unit");
        check(`records.csv unit ${u.unit}: time rules (section 5)`,
          prev?.ts === "" && boot?.ts_src === "uptime" && bootErr < 15_000 && late !== undefined,
          `prev ts "${prev?.ts}", boot ${boot?.ts_src} off by ${bootErr} ms`);
      }
      const extra = await fetch(`${base}/api/export/records.csv?unit=${fleet[0].unit}`, { headers: { Cookie: cookie } });
      const extraRows = parseCsv(await extra.text()).filter((r) => Number(r.seq) >= SEQ_BASE);
      const cad = extraRows.find((r) => r.k === "cad");
      const fe = extraRows.find((r) => r.k === "rx" && r.id === "4242");
      check("unknown kind and unknown field kept in extra",
        cad?.extra.includes('"busy_pm":7') && fe?.extra.includes('"freq_err":-812') && fe?.rssi === "-101",
        `cad extra ${cad?.extra}, rx extra ${fe?.extra}`);
      const bySeq = (seq) => extraRows.find((r) => Number(r.seq) === seq);
      const odd = {
        wrongType: bySeq(oddSeqs.wrongType), gps: bySeq(oddSeqs.gps), chAll: bySeq(oddSeqs.chAll),
        protoNames: bySeq(oddSeqs.protoNames),
      };
      const extraOf = (r) => {
        try {
          return JSON.parse(r?.extra || "null") ?? {};
        } catch {
          return {};
        }
      };
      const wt = extraOf(odd.wrongType);
      const pn = extraOf(odd.protoNames);
      check("wrong-typed fields kept in extra with _type_errors",
        odd.wrongType?.hops === "" && odd.wrongType?.rssi === "-99" && wt.hops === "two" &&
          wt._type_errors?.[0] === "hops" && extraOf(odd.gps).n === 1.5 && odd.gps?.n === "" &&
          extraOf(odd.chAll).ch === "ALL",
        `rx ${odd.wrongType?.extra}, gps ${odd.gps?.extra}, ch ${odd.chAll?.extra}`);
      check("fields named constructor, toString, __proto__ kept in extra",
        Object.hasOwn(pn, "constructor") && pn.constructor === 5 && Object.hasOwn(pn, "toString") &&
          pn.toString === "x" && Object.hasOwn(pn, "__proto__") && pn.newfield === 1,
        `extra ${odd.protoNames?.extra}`);
      check("records.csv carries the board's mac", extraRows.length > 0 && extraRows.every((r) => r.mac === fleet[0].mac),
        `macs ${[...new Set(extraRows.map((r) => r.mac))].join(" ")}`);

      // The collisions: kept in conflicts.csv with the sending board's MAC,
      // once each despite the re-send; records.csv still has A's own rows.
      const cf = await fetch(`${base}/api/export/conflicts.csv?unit=${fleet[0].unit}`, { headers: { Cookie: cookie } });
      const cfRows = parseCsv(await cf.text()).filter((r) => Number(r.seq) >= SEQ_BASE);
      const spareRow = cfRows.find((r) => Number(r.seq) === collided[0]);
      const erasedRow = cfRows.find((r) => Number(r.seq) === collided[1]);
      check("conflicts.csv keeps each collision once, with its board's mac",
        cf.ok && cfRows.length === 2 && spareRow?.mac === SPARE_MAC && spareRow?.rssi === "-110" &&
          erasedRow?.mac === fleet[0].mac && erasedRow?.batt_pct === "5" &&
          bySeq(collided[0])?.k === "boot" && bySeq(collided[1])?.boot === String(fleet[0].boot),
        `HTTP ${cf.status}, ${cfRows.length} rows: ${cfRows.map((r) => `${r.seq} ${r.mac}`).join("; ")}`);
      const ov = await fetch(`${base}/api/overview`, { headers: { Cookie: cookie } });
      const ovA = (await ov.json().catch(() => null))?.units?.find((x) => x.unit === fleet[0].unit);
      check("admin overview flags the unit (two boards, collisions)",
        ov.ok && ovA?.conflicts >= 2 && ovA?.boards?.includes(SPARE_MAC) && ovA?.boards?.includes(fleet[0].mac),
        `conflicts ${ovA?.conflicts}, boards ${ovA?.boards?.join(" ")}`);
      const bogus = await fetch(`${base}/api/export/constructor`, { headers: { Cookie: cookie } });
      check("export named like an Object.prototype member is 404", bogus.status === 404, `HTTP ${bogus.status}`);

      const rw = await fetch(`${base}/api/export/readings_with_location.csv`, { headers: { Cookie: cookie } });
      const readings = parseCsv(await rw.text()).filter((r) => Number(r.seq) >= SEQ_BASE);
      const withDist = readings.filter((r) => r.distance_m !== "");
      check("readings_with_location.csv has rx rows", rw.ok && readings.length > 0, `${readings.length} rows`);
      if (passcodes) {
        check("readings_with_location.csv has distances", withDist.length > 0,
          `${withDist.length} with distance, e.g. ${withDist[0]?.unit}<-${withDist[0]?.from} ${withDist[0]?.distance_m} m`);
      }
      for (const f of ["locations.csv", "notes.csv"]) {
        const r = await fetch(`${base}/api/export/${f}`, { headers: { Cookie: cookie } });
        const text = await r.text();
        check(`${f} downloads`, r.ok && text.split("\r\n")[0].startsWith("unit,"), `HTTP ${r.status}`);
      }
    }
  }

  const failed = results.filter((r) => !r.ok).length;
  console.log(`\n[sim] ${results.length - failed} passed, ${failed} failed`);
  process.exit(failed ? 1 : 0);
}

main().catch((err) => {
  console.error(`[sim] ${err.stack ?? err}`);
  process.exit(1);
});
