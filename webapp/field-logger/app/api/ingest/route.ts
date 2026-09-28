/**
 * POST /api/ingest: unit uploads (docs/field-logger-protocol.md sections 1-5).
 *
 * Order of checks: size, unit header, signature, then JSON. The signature is
 * checked on the raw bytes before anything parses them, so unauthenticated
 * bodies are never interpreted. Any non-200 makes the unit keep its records
 * and retry next window, so a 5xx loses nothing.
 */
import { getDb } from "@/lib/db";
import { isUnit, logKey } from "@/lib/config";
import { verifyUploadSignature } from "@/lib/crypto";
import { json, readBodyLimited } from "@/lib/http";
import { BadRequest, MAX_BODY_BYTES, parseBody, storeUpload, toRow } from "@/lib/ingest";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function POST(req: Request): Promise<Response> {
  const receivedAtMs = Date.now();
  const serverEpoch = Math.floor(receivedAtMs / 1000);

  const key = logKey();
  if (!key) {
    console.error("[ingest] LOG_KEY is not set or not 32-64 hex characters");
    return json({ ok: false, error: "server has no log key" }, 503);
  }

  const raw = await readBodyLimited(req, MAX_BODY_BYTES);
  if (!raw) return json({ ok: false, error: `body over ${MAX_BODY_BYTES} bytes` }, 413);

  const header = req.headers.get("x-fm-unit") ?? "";
  const unit = header.trim();
  if (!isUnit(unit)) {
    console.warn(`[ingest] refused unit "${header.slice(0, 16)}"`);
    return json({ ok: false, error: "unit not allowed" }, 401);
  }
  const sig = req.headers.get("x-fm-signature") ?? "";
  if (!verifyUploadSignature(raw, sig, key)) {
    console.warn(`[ingest] ${unit}: bad signature (${raw.length} bytes)`);
    return json({ ok: false, error: "bad signature" }, 401);
  }

  let parsed: unknown;
  try {
    parsed = JSON.parse(raw.toString("utf8"));
  } catch {
    return json({ ok: false, error: "malformed JSON" }, 400);
  }

  try {
    const { env, records } = parseBody(parsed);
    if (env.unit !== unit) {
      console.warn(`[ingest] ${unit}: body says unit "${env.unit}"`);
      return json({ ok: false, error: "unit in body differs from X-FM-Unit" }, 401);
    }
    const problems: string[] = [];
    const rows = records.map((r, i) => toRow(r, i, env, receivedAtMs, problems));
    // Fields kept in extra because their type was wrong: stored, but a
    // firmware bug, so it goes in the log where the bench will see it.
    for (const p of problems.slice(0, 5)) console.warn(`[ingest] ${unit}: ${p}`);
    if (problems.length > 5) console.warn(`[ingest] ${unit}: ... and ${problems.length - 5} more fields kept in extra`);
    const db = await getDb();
    const res = await storeUpload(db, env, rows, receivedAtMs, raw.length);
    console.log(
      `[ingest] ${unit}: ${rows.length} records (${res.inserted} new, ${res.duplicates} dup` +
        `${res.conflicts ? `, ${res.conflicts} seq collisions` : ""}), ` +
        `acked ${res.ackedSeq ?? "-"}, boot ${env.boot}, fw ${env.fw ?? "?"}`,
    );
    if (res.conflict) {
      const c = res.conflict;
      console.warn(
        `[ingest] ${unit}: ${res.conflicts} seq collisions (seq ${c.firstSeq}-${c.lastSeq}: stored boot ` +
          `${c.storedBoot ?? "?"} mac ${c.storedMac ?? "?"}, sent boot ${env.boot} mac ${env.mac ?? "?"}), ` +
          `kept in record_conflicts (${c.newlyKept} new). Flash erased, or another board has this call sign?`,
      );
    }
    return json({
      ok: true,
      acked_seq: res.ackedSeq,
      server_epoch: serverEpoch,
      // Not part of the unit's contract; lets the simulator and a curl user
      // see that a re-send stored nothing twice and that a reused seq was
      // kept apart rather than dropped.
      inserted: res.inserted,
      duplicates: res.duplicates,
      conflicts: res.conflicts,
    });
  } catch (err) {
    if (err instanceof BadRequest) {
      console.warn(`[ingest] ${unit}: 400 ${err.message}`);
      return json({ ok: false, error: err.message }, 400);
    }
    console.error(`[ingest] ${unit}: ${(err as Error).message}`);
    return json({ ok: false, error: "server error" }, 500);
  }
}
