/**
 * Admin cost sheet.
 *   GET    the full sheet (component lines, markup) and its revision
 *   PUT    replace it: JSON { sheet, expectedRev, force? }
 *   DELETE go back to the built-in defaults: ?expectedRev=<n|none>[&force=1]
 *
 * A write whose expectedRev is not the stored revision gets 409 with the
 * current sheet, so a stale tab cannot silently overwrite a newer save.
 */
import { publicCosts, validateCosts } from "@/lib/costs";
import { loadCosts, resetCosts, saveCosts } from "@/lib/costStore";
import { json, readJson, requireAdmin, sameOrigin } from "@/lib/http";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const DB_DOWN = "The database is unavailable right now; nothing was changed. Try again in a minute.";

async function current(status = 200, extra: Record<string, unknown> = {}): Promise<Response> {
  const c = await loadCosts();
  return json(
    {
      ok: status === 200,
      ...extra,
      sheet: c.sheet,
      updatedAt: c.updatedAt,
      isDefault: c.isDefault,
      rev: c.rev,
      public: publicCosts(c.sheet, c.updatedAt, c.isDefault),
    },
    status,
  );
}

/** expectedRev from the client: a non-negative integer, or null for "I saw the defaults". */
function parseRev(v: unknown): number | null | undefined {
  if (v === null || v === "none") return null;
  const n = typeof v === "number" ? v : typeof v === "string" && /^\d{1,15}$/.test(v) ? Number(v) : NaN;
  return Number.isSafeInteger(n) && n >= 0 ? n : undefined;
}

const CONFLICT = "The price list was changed elsewhere (another tab or another admin) since this page loaded.";

export async function GET(): Promise<Response> {
  const s = await requireAdmin();
  if (s instanceof Response) return s;
  try {
    return await current();
  } catch (err) {
    console.error("[costs] load failed", err);
    return json({ ok: false, error: DB_DOWN }, 503);
  }
}

export async function PUT(req: Request): Promise<Response> {
  const s = await requireAdmin();
  if (s instanceof Response) return s;
  if (!sameOrigin(req)) return json({ ok: false, error: "Request from another site refused." }, 403);
  const body = await readJson(req, 64 * 1024);
  if (!body) return json({ ok: false, error: "Expected a JSON body under 64 KB." }, 400);
  const v = validateCosts(body.sheet);
  if (!v.ok) return json({ ok: false, error: v.error }, 400);
  const expectedRev = parseRev(body.expectedRev);
  if (expectedRev === undefined) return json({ ok: false, error: "expectedRev missing or invalid." }, 400);
  let saved: boolean;
  try {
    saved = await saveCosts(v.sheet, expectedRev, body.force === true);
  } catch (err) {
    console.error("[costs] save failed", err);
    return json({ ok: false, error: DB_DOWN }, 503);
  }
  try {
    if (!saved) return await current(409, { conflict: true, error: CONFLICT });
    console.log("[costs] sheet updated by admin");
    return await current();
  } catch (err) {
    console.error("[costs] re-read after save failed", err);
    return json({ ok: false, error: "Saved, but the sheet could not be read back. Reload the page." }, 503);
  }
}

export async function DELETE(req: Request): Promise<Response> {
  const s = await requireAdmin();
  if (s instanceof Response) return s;
  if (!sameOrigin(req)) return json({ ok: false, error: "Request from another site refused." }, 403);
  const url = new URL(req.url);
  const expectedRev = parseRev(url.searchParams.get("expectedRev"));
  if (expectedRev === undefined) return json({ ok: false, error: "expectedRev missing or invalid." }, 400);
  let done: boolean;
  try {
    done = await resetCosts(expectedRev, url.searchParams.get("force") === "1");
  } catch (err) {
    console.error("[costs] reset failed", err);
    return json({ ok: false, error: DB_DOWN }, 503);
  }
  try {
    if (!done) return await current(409, { conflict: true, error: CONFLICT });
    console.log("[costs] sheet reset to defaults by admin");
    return await current();
  } catch (err) {
    console.error("[costs] re-read after reset failed", err);
    return json({ ok: false, error: "Reset, but the sheet could not be read back. Reload the page." }, 503);
  }
}
