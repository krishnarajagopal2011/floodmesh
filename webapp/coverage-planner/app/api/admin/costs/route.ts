/**
 * Admin cost sheet.
 *   GET    the full sheet (component lines, markup)
 *   PUT    replace it (JSON body: the sheet)
 *   DELETE go back to the built-in defaults
 */
import { publicCosts, validateCosts } from "@/lib/costs";
import { loadCosts, resetCosts, saveCosts } from "@/lib/costStore";
import { json, readJson, requireAdmin, sameOrigin } from "@/lib/http";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

async function current(): Promise<Response> {
  const c = await loadCosts();
  return json({
    ok: true,
    sheet: c.sheet,
    updatedAt: c.updatedAt,
    isDefault: c.isDefault,
    public: publicCosts(c.sheet, c.updatedAt, c.isDefault),
  });
}

export async function GET(): Promise<Response> {
  const s = await requireAdmin();
  if (s instanceof Response) return s;
  return current();
}

export async function PUT(req: Request): Promise<Response> {
  const s = await requireAdmin();
  if (s instanceof Response) return s;
  if (!sameOrigin(req)) return json({ ok: false, error: "Request from another site refused." }, 403);
  const body = await readJson(req, 64 * 1024);
  if (!body) return json({ ok: false, error: "Expected a JSON cost sheet under 64 KB." }, 400);
  const v = validateCosts(body);
  if (!v.ok) return json({ ok: false, error: v.error }, 400);
  await saveCosts(v.sheet);
  console.log("[costs] sheet updated by admin");
  return current();
}

export async function DELETE(req: Request): Promise<Response> {
  const s = await requireAdmin();
  if (s instanceof Response) return s;
  if (!sameOrigin(req)) return json({ ok: false, error: "Request from another site refused." }, 403);
  await resetCosts();
  console.log("[costs] sheet reset to defaults by admin");
  return current();
}
