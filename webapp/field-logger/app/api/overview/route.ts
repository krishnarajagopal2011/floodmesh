/** GET /api/overview: all units at a glance, admin only. */
import { json, requireSession } from "@/lib/http";
import { overview } from "@/lib/queries";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET(): Promise<Response> {
  const s = await requireSession();
  if (s instanceof Response) return s;
  if (s.role !== "admin") return json({ ok: false, error: "admin only" }, 403);
  try {
    return json({ ok: true, ...(await overview()) });
  } catch (err) {
    console.error(`[overview] ${(err as Error).message}`);
    return json({ ok: false, error: "server error" }, 500);
  }
}
