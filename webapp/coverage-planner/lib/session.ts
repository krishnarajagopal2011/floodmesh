/**
 * Admin session cookie: base64url(JSON payload) + "." + base64url(HMAC-SHA256).
 * Same scheme as the field logger, so it works across Vercel instances without
 * a session table. 8 hours covers a working day or a presentation.
 *
 * The MAC key is derived from SESSION_SECRET, this app's name and a hash of
 * ADMIN_PASSWORD. So:
 * - changing ADMIN_PASSWORD (and redeploying) logs every admin out at once,
 *   with no session table to clear;
 * - a field-logger cookie signed with the same SESSION_SECRET is not
 *   accepted here.
 */
import { cookies } from "next/headers";
import { createHash, timingSafeEqual } from "node:crypto";
import { adminPassword, sessionSecret } from "./config";
import { hmacSha256 } from "./crypto";

export const SESSION_COOKIE = "fm_planner_session";
export const SESSION_TTL_S = 8 * 60 * 60;

export type Session = { role: "admin"; exp: number };

/** Null when ADMIN_PASSWORD is unset: then no session is valid. */
function sessionKey(): Buffer | null {
  const pw = adminPassword();
  if (!pw) return null;
  const pwHash = createHash("sha256").update(pw, "utf8").digest("hex");
  return hmacSha256(sessionSecret(), `floodmesh-coverage-planner|session|${pwHash}`);
}

export function signSession(s: Session): string {
  const key = sessionKey();
  if (!key) throw new Error("ADMIN_PASSWORD is not set");
  const payload = Buffer.from(JSON.stringify(s), "utf8").toString("base64url");
  const mac = hmacSha256(key, payload).toString("base64url");
  return `${payload}.${mac}`;
}

export function verifySession(token: string | undefined | null): Session | null {
  if (!token) return null;
  const dot = token.indexOf(".");
  if (dot <= 0) return null;
  const payload = token.slice(0, dot);
  let given: Buffer;
  try {
    given = Buffer.from(token.slice(dot + 1), "base64url");
  } catch {
    return null;
  }
  const key = sessionKey();
  if (!key) return null;
  const expected = hmacSha256(key, payload);
  if (given.length !== expected.length || !timingSafeEqual(given, expected)) return null;
  try {
    const s = JSON.parse(Buffer.from(payload, "base64url").toString("utf8")) as Session;
    if (s.role !== "admin" || typeof s.exp !== "number" || s.exp * 1000 < Date.now()) return null;
    return s;
  } catch {
    return null;
  }
}

/** Session of the current request (server components and route handlers). */
export async function getSession(): Promise<Session | null> {
  const jar = await cookies();
  return verifySession(jar.get(SESSION_COOKIE)?.value);
}

/**
 * Cookie attributes. `secure` whenever the request came over HTTPS (always on
 * Vercel); a production build served over plain HTTP on a laptop for a
 * presentation would otherwise never keep its login.
 */
export function cookieOptions(secure: boolean, maxAge = SESSION_TTL_S) {
  return { httpOnly: true, secure, sameSite: "lax" as const, path: "/", maxAge };
}
