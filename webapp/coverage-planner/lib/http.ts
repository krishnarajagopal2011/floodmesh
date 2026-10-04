/** Small helpers shared by the route handlers (same as the field logger's). */
import { trustProxy } from "./config";
import { getSession, type Session } from "./session";

export function json(body: unknown, status = 200, headers: Record<string, string> = {}): Response {
  return Response.json(body, { status, headers: { "Cache-Control": "no-store", ...headers } });
}

export function isHttps(req: Request): boolean {
  const proto = req.headers.get("x-forwarded-proto");
  if (proto) return proto.split(",")[0].trim() === "https";
  return new URL(req.url).protocol === "https:";
}

/** Secure cookies in production whenever the connection allows it (see lib/session.ts). */
export function secureCookies(req: Request): boolean {
  return process.env.NODE_ENV === "production" && (isHttps(req) || Boolean(process.env.VERCEL));
}

/**
 * The caller's address, for the per-IP login limit. On Vercel the platform
 * writes X-Forwarded-For; elsewhere it is trusted only behind a proxy that
 * TRUST_PROXY names, otherwise every caller shares one "direct" bucket.
 */
export function clientIp(req: Request): string {
  const xff = (req.headers.get("x-forwarded-for") ?? "")
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean);
  if (process.env.VERCEL) return xff[0] ?? req.headers.get("x-real-ip") ?? "unknown";
  if (trustProxy() && xff.length > 0) return xff[xff.length - 1];
  return "direct";
}

/**
 * Cross-site request guard for cookie-authenticated writes. Browsers send
 * Origin on every POST/PUT; a mismatch means another site is submitting with
 * the admin's cookie. Requests without Origin (curl) carry no ambient cookie.
 */
export function sameOrigin(req: Request): boolean {
  const origin = req.headers.get("origin");
  if (!origin) return true;
  const host = req.headers.get("x-forwarded-host") ?? req.headers.get("host");
  try {
    return new URL(origin).host === host;
  } catch {
    return false;
  }
}

/** Read the request body, stopping as soon as it exceeds `limit` bytes. Null when too large. */
export async function readBodyLimited(req: Request, limit: number): Promise<Buffer | null> {
  const declared = Number(req.headers.get("content-length"));
  if (Number.isFinite(declared) && declared > limit) return null;
  if (!req.body) return Buffer.alloc(0);
  const reader = req.body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > limit) {
      await reader.cancel().catch(() => {});
      return null;
    }
    chunks.push(value);
  }
  return Buffer.concat(chunks, total);
}

/** The admin session, or a 401 response to return as is. */
export async function requireAdmin(): Promise<Session | Response> {
  const s = await getSession();
  return s ?? json({ ok: false, error: "not logged in" }, 401);
}

/** Parse a JSON object body. */
export async function readJson(req: Request, limit = 4096): Promise<Record<string, unknown> | null> {
  const raw = await readBodyLimited(req, limit);
  if (!raw) return null;
  try {
    const v = JSON.parse(raw.toString("utf8")) as unknown;
    return v && typeof v === "object" && !Array.isArray(v) ? (v as Record<string, unknown>) : null;
  } catch {
    return null;
  }
}
