/**
 * Session cookie: base64url(JSON payload) + "." + base64url(HMAC-SHA256).
 *
 * Stateless, so it works across Vercel instances without a session table.
 * A volunteer logs in once at the start of a test day; 12 hours covers the
 * day without leaving a phone signed in overnight.
 */
import { cookies } from "next/headers";
import { hmacSha256 } from "./crypto";
import { isUnit, sessionSecret } from "./config";
import { timingSafeEqual } from "node:crypto";

export const SESSION_COOKIE = "fm_session";
export const SESSION_TTL_S = 12 * 60 * 60;

export type Session =
  | { role: "unit"; unit: string; exp: number }
  | { role: "admin"; exp: number };

export function signSession(s: Session): string {
  const payload = Buffer.from(JSON.stringify(s), "utf8").toString("base64url");
  const mac = hmacSha256(sessionSecret(), payload).toString("base64url");
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
  const expected = hmacSha256(sessionSecret(), payload);
  if (given.length !== expected.length || !timingSafeEqual(given, expected)) return null;
  try {
    const s = JSON.parse(Buffer.from(payload, "base64url").toString("utf8")) as Session;
    if (typeof s.exp !== "number" || s.exp * 1000 < Date.now()) return null;
    if (s.role === "admin") return s;
    // A unit removed from UNITS loses access at once.
    if (s.role === "unit" && isUnit(s.unit)) return s;
    return null;
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
 * Cookie attributes. `secure` whenever the request came over HTTPS (always
 * on Vercel); a production build served over plain HTTP on the LAN for a
 * bench test would otherwise never keep its login.
 */
export function cookieOptions(secure: boolean, maxAge = SESSION_TTL_S) {
  return {
    httpOnly: true,
    secure,
    sameSite: "lax" as const,
    path: "/",
    maxAge,
  };
}

/** Which units a session may read and write. */
export function canAccessUnit(s: Session, unit: string): boolean {
  return s.role === "admin" ? isUnit(unit) : s.unit === unit;
}
