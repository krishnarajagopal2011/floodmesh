/**
 * GET /api/costs: the public price list the planner uses (one price per unit
 * kind and one rate per project line, markup included, no breakdown).
 */
import { publicCosts } from "@/lib/costs";
import { loadCosts } from "@/lib/costStore";
import { json } from "@/lib/http";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET(): Promise<Response> {
  try {
    const c = await loadCosts();
    return json({ ok: true, costs: publicCosts(c.sheet, c.updatedAt, c.isDefault) });
  } catch (err) {
    console.error("[costs] load failed", err);
    return json({ ok: false, error: "Costs are unavailable right now." }, 503);
  }
}
