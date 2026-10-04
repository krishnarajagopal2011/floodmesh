/**
 * Database schema for the coverage planner. Idempotent statements run on
 * first use by every server process. Plain SQL that both Neon Postgres and
 * PGlite accept.
 */
export const SCHEMA: readonly string[] = [
  // Key-value settings. "costs" holds the cost sheet edited on /admin.
  `CREATE TABLE IF NOT EXISTS settings (
    key        text PRIMARY KEY,
    value      jsonb       NOT NULL,
    updated_at timestamptz NOT NULL DEFAULT now()
  )`,
  // Revision of each setting, bumped on every save (lib/costStore.ts).
  `ALTER TABLE settings ADD COLUMN IF NOT EXISTS rev bigint NOT NULL DEFAULT 0`,
  // Login rate limiting (lib/ratelimit.ts).
  `CREATE TABLE IF NOT EXISTS login_limits (
    key          text PRIMARY KEY,
    window_start timestamptz NOT NULL,
    n            integer     NOT NULL
  )`,
];
