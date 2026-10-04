/**
 * Settings from the environment (README lists them all).
 *
 * Read on every call rather than cached at import, so a missing secret fails
 * the request that needs it with a clear message instead of failing the build.
 */

/** The admin password for /admin. Unset = nobody can log in. */
export function adminPassword(): string | undefined {
  const v = process.env.ADMIN_PASSWORD;
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
 * TRUST_PROXY=1: a reverse proxy in front of a self-hosted server appends the
 * client's address to X-Forwarded-For (see clientIp in lib/http.ts). Not
 * needed on Vercel, which sets that header itself.
 */
export function trustProxy(): boolean {
  const v = (process.env.TRUST_PROXY ?? "").trim().toLowerCase();
  return v === "1" || v === "true" || v === "yes";
}
