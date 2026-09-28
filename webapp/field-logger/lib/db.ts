/**
 * Database adapter.
 *
 * Production (Vercel): Neon Postgres over its HTTP driver, selected by
 * DATABASE_URL. Serverless functions have no long-lived connections, and one
 * HTTP round trip per query suits the handful of queries a request makes.
 *
 * Local development and tests: PGlite, a real Postgres compiled to WASM that
 * stores its files in ./.pglite (or PGLITE_DIR). No server to install, and the
 * SQL is exactly the SQL that runs on Neon.
 *
 * The schema (lib/schema.ts) is applied once per process on first use.
 * Imported by scripts/migrate.ts under Node's type stripping, so imports here
 * are relative with explicit extensions.
 */
import path from "node:path";
import { SCHEMA } from "./schema.ts";

export type Row = Record<string, unknown>;

export interface Db {
  readonly kind: "neon" | "pglite";
  query<T extends Row = Row>(text: string, params?: unknown[]): Promise<T[]>;
  close(): Promise<void>;
}

// Kept on globalThis so `next dev` hot reloads reuse one PGlite instance;
// two instances on the same directory would corrupt it.
const g = globalThis as unknown as { __fmDb?: Promise<Db> };

export function getDb(): Promise<Db> {
  if (!g.__fmDb) {
    g.__fmDb = open().catch((err) => {
      g.__fmDb = undefined; // let the next request try again
      throw err;
    });
  }
  return g.__fmDb;
}

/** Convenience for route handlers. */
export async function query<T extends Row = Row>(text: string, params?: unknown[]): Promise<T[]> {
  const db = await getDb();
  return db.query<T>(text, params);
}

async function open(): Promise<Db> {
  const db = process.env.DATABASE_URL ? await openNeon(process.env.DATABASE_URL) : await openPglite();
  await migrate(db);
  return db;
}

async function openNeon(url: string): Promise<Db> {
  const { neon } = await import("@neondatabase/serverless");
  const sql = neon(url);
  return {
    kind: "neon",
    query: async <T extends Row>(text: string, params: unknown[] = []) =>
      (await sql.query(text, params)) as unknown as T[],
    close: async () => {},
  };
}

async function openPglite(): Promise<Db> {
  if (process.env.VERCEL) {
    // Vercel's filesystem is read-only and per-instance: an embedded database
    // there would silently lose every reading. Refuse loudly instead.
    throw new Error("DATABASE_URL is not set. Add a Neon database to this Vercel project (see README).");
  }
  const { PGlite } = await import("@electric-sql/pglite");
  // Local-only path: keep the bundler from tracing the whole project for it.
  const dir = path.resolve(/*turbopackIgnore: true*/ process.env.PGLITE_DIR || path.join(process.cwd(), ".pglite"));
  const pg = await PGlite.create(dir);
  console.log(`[db] PGlite at ${dir}`);
  return {
    kind: "pglite",
    query: async <T extends Row>(text: string, params: unknown[] = []) =>
      (await pg.query<T>(text, params)).rows,
    close: () => pg.close(),
  };
}

/**
 * Apply the schema. Every statement is idempotent. Two serverless instances
 * starting at the same moment can race on CREATE TABLE IF NOT EXISTS (a
 * catalog unique-key error in one of them), so a failure is retried once.
 */
export async function migrate(db: Db): Promise<void> {
  for (const stmt of SCHEMA) {
    try {
      await db.query(stmt);
    } catch (err) {
      await new Promise((r) => setTimeout(r, 300));
      await db.query(stmt).catch(() => {
        throw err;
      });
    }
  }
}

// ---------------------------------------------------------------- value helpers
// Neon returns int8/numeric as strings, PGlite as numbers or bigint; both
// return timestamptz as Date. These give route handlers one shape.

export function num(v: unknown): number | null {
  if (v === null || v === undefined) return null;
  const n = typeof v === "number" ? v : Number(v);
  return Number.isFinite(n) ? n : null;
}

export function iso(v: unknown): string | null {
  if (v === null || v === undefined) return null;
  const d = v instanceof Date ? v : new Date(String(v));
  return Number.isNaN(d.getTime()) ? null : d.toISOString();
}

export function str(v: unknown): string | null {
  return v === null || v === undefined ? null : String(v);
}
