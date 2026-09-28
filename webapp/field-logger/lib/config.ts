/**
 * Settings from the environment (README lists them all).
 *
 * Read on every call rather than cached at import, so a missing secret fails
 * the request that needs it with a clear message instead of failing the build.
 */

/** Call signs allowed to upload and log in. Default: the five test units. */
export function units(): string[] {
  const raw = process.env.UNITS ?? "A,B,C,D,E";
  const list = raw
    .split(",")
    .map((s) => s.trim())
    .filter((s) => s.length > 0 && s.length <= 16);
  return Array.from(new Set(list));
}

export function isUnit(u: unknown): u is string {
  return typeof u === "string" && units().includes(u);
}

/** Per-unit passcodes from UNIT_PASSCODES, JSON such as {"A":"...","B":"..."}. */
export function unitPasscode(unit: string): string | undefined {
  const raw = process.env.UNIT_PASSCODES;
  if (!raw) return undefined;
  try {
    const map = JSON.parse(raw) as Record<string, unknown>;
    const v = map?.[unit];
    return typeof v === "string" && v.length > 0 ? v : undefined;
  } catch {
    console.error("[config] UNIT_PASSCODES is not valid JSON");
    return undefined;
  }
}

export function adminPasscode(): string | undefined {
  const v = process.env.ADMIN_PASSCODE;
  return v && v.length > 0 ? v : undefined;
}

/** Signs session cookies. Required; short secrets are refused. */
export function sessionSecret(): string {
  const v = process.env.SESSION_SECRET;
  if (!v || v.length < 32) {
    throw new Error("SESSION_SECRET is missing or shorter than 32 characters (see README)");
  }
  return v;
}

/**
 * Fleet log key (protocol section 6): hex, 32-64 characters, same on every
 * unit. An even length is required because the firmware decodes it to bytes
 * and refuses an odd-length key.
 */
export function logKey(): string | undefined {
  const v = process.env.LOG_KEY?.trim();
  if (!v) return undefined;
  if (!/^(?:[0-9a-fA-F]{2}){16,32}$/.test(v)) {
    console.error("[config] LOG_KEY must be 32-64 hex characters, an even number of them");
    return undefined;
  }
  return v;
}

/**
 * TRUST_PROXY=1: a reverse proxy in front of a self-hosted server appends the
 * client's address to X-Forwarded-For (see clientIp in lib/http.ts). Not
 * needed on Vercel, which sets that header itself.
 */
export function trustProxy(): boolean {
  const v = (process.env.TRUST_PROXY ?? "").trim().toLowerCase();
  return v === "1" || v === "true" || v === "yes";
}

/** Time zone for "today" on the admin page. The tests run in India. */
export function timeZone(): string {
  return process.env.TIMEZONE || "Asia/Kolkata";
}

export const isProduction = process.env.NODE_ENV === "production";
