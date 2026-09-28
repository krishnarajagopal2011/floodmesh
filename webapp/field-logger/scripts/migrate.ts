/**
 * npm run db:migrate: create or upgrade the tables (lib/schema.ts).
 *
 * Uses DATABASE_URL when set (Neon), else the local PGlite folder. The app
 * also does this on first use, so running it is optional; it is handy to
 * check a new DATABASE_URL before the field day. Do not run it against
 * PGlite while `npm run dev` or `npm start` holds the same folder open.
 *
 * Runs under Node's built-in TypeScript type stripping (Node >= 22.18).
 */
import { getDb } from "../lib/db.ts";

const db = await getDb();
const tables = await db.query<{ table_name: string }>(
  `SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' ORDER BY table_name`,
);
console.log(`[migrate] schema applied on ${db.kind}: ${tables.map((t) => t.table_name).join(", ")}`);
await db.close();
