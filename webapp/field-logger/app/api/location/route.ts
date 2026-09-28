/**
 * POST /api/location {lat, lon, accuracy, altitude?, ts?}: the volunteer's
 * phone position, sent by the page only after the volunteer agreed to share.
 *
 * Only a unit login may post, and only for its own unit: the position stands
 * in for the unit's (units have no GPS), so the admin's phone must not.
 */
import { query } from "@/lib/db";
import { json, readJson, requireSession, sameOrigin } from "@/lib/http";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

// The page sends at most every 20 s; the server refuses anything much faster
// so a stuck or hostile client cannot flood the table.
const MIN_INTERVAL_S = 5;
// Phone clocks are network-synced, so the fix time is used unless it is
// implausibly far from the server's clock.
const MAX_CLOCK_SKEW_MS = 10 * 60_000;

function finite(v: unknown): v is number {
  return typeof v === "number" && Number.isFinite(v);
}

export async function POST(req: Request): Promise<Response> {
  if (!sameOrigin(req)) return json({ ok: false, error: "cross-site request refused" }, 403);
  const s = await requireSession();
  if (s instanceof Response) return s;
  if (s.role !== "unit") return json({ ok: false, error: "only a unit login shares its location" }, 403);

  const b = await readJson(req);
  if (!b) return json({ ok: false, error: "expected a JSON object" }, 400);
  const { lat, lon, accuracy, altitude, ts } = b;
  if (!finite(lat) || lat < -90 || lat > 90 || !finite(lon) || lon < -180 || lon > 180) {
    return json({ ok: false, error: "lat/lon out of range" }, 400);
  }
  const acc = finite(accuracy) && accuracy >= 0 ? accuracy : null;
  const alt = finite(altitude) ? altitude : null;
  const now = Date.now();
  const fixMs = finite(ts) && Math.abs(ts - now) <= MAX_CLOCK_SKEW_MS ? ts : now;

  try {
    const recent = await query(
      `SELECT 1 FROM locations WHERE unit = $1 AND received_at > now() - make_interval(secs => $2) LIMIT 1`,
      [s.unit, MIN_INTERVAL_S],
    );
    if (recent.length > 0) {
      return json({ ok: false, error: "too frequent" }, 429, { "Retry-After": String(MIN_INTERVAL_S) });
    }
    await query(
      `INSERT INTO locations (unit, ts, lat, lon, accuracy, altitude) VALUES ($1, $2, $3, $4, $5, $6)`,
      [s.unit, new Date(fixMs).toISOString(), lat, lon, acc, alt],
    );
    return json({ ok: true, ts: new Date(fixMs).toISOString(), accuracy: acc });
  } catch (err) {
    console.error(`[location] ${s.unit}: ${(err as Error).message}`);
    return json({ ok: false, error: "server error" }, 500);
  }
}
