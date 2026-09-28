/**
 * GET /api/status?unit=A: everything the volunteer page shows for one unit.
 * A unit login sees only its own unit; the admin may ask for any.
 */
import { json, requireSession } from "@/lib/http";
import { unitStatus } from "@/lib/queries";
import { canAccessUnit } from "@/lib/session";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET(req: Request): Promise<Response> {
  const s = await requireSession();
  if (s instanceof Response) return s;
  const asked = new URL(req.url).searchParams.get("unit");
  const unit = asked ?? (s.role === "unit" ? s.unit : null);
  if (!unit) return json({ ok: false, error: "unit required" }, 400);
  if (!canAccessUnit(s, unit)) return json({ ok: false, error: "not your unit" }, 403);
  try {
    return json({ ok: true, ...(await unitStatus(unit)) });
  } catch (err) {
    console.error(`[status] ${unit}: ${(err as Error).message}`);
    return json({ ok: false, error: "server error" }, 500);
  }
}
