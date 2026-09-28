/**
 * POST /api/note {text, unit?}: a free-text observation for the analysis
 * ("moved to the terrace", "unit dropped in water"). A unit login writes to
 * its own unit; the admin names the unit.
 */
import { isUnit } from "@/lib/config";
import { iso, query } from "@/lib/db";
import { json, readJson, requireSession, sameOrigin } from "@/lib/http";
import { canAccessUnit } from "@/lib/session";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const MAX_NOTE = 1000;

export async function POST(req: Request): Promise<Response> {
  if (!sameOrigin(req)) return json({ ok: false, error: "cross-site request refused" }, 403);
  const s = await requireSession();
  if (s instanceof Response) return s;

  const b = await readJson(req, 8192);
  if (!b) return json({ ok: false, error: "expected a JSON object" }, 400);
  const text = typeof b.text === "string" ? b.text.replaceAll("\u0000", "").trim() : "";
  if (!text) return json({ ok: false, error: "note is empty" }, 400);
  if (text.length > MAX_NOTE) return json({ ok: false, error: `note longer than ${MAX_NOTE} characters` }, 400);
  const unit = s.role === "unit" ? s.unit : b.unit;
  if (!isUnit(unit) || !canAccessUnit(s, unit)) return json({ ok: false, error: "unit not allowed" }, 403);

  try {
    const rows = await query(
      `INSERT INTO notes (unit, author, text) VALUES ($1, $2, $3) RETURNING ts`,
      [unit, s.role, text],
    );
    return json({ ok: true, ts: iso(rows[0]?.ts) });
  } catch (err) {
    console.error(`[note] ${unit}: ${(err as Error).message}`);
    return json({ ok: false, error: "server error" }, 500);
  }
}
