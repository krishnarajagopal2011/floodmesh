/**
 * Reads and writes the cost sheet in the settings table. Server only.
 *
 * Every save bumps a revision number, and a save or reset says which revision
 * the editor started from. If someone else saved in between (another tab, a
 * second admin), the write is refused as a conflict instead of silently
 * overwriting their newer sheet. `force` overwrites anyway.
 *
 * A reset keeps the row, with a JSON null as its value and the revision
 * bumped, rather than deleting it: revision numbers are never reused, so a
 * tab still holding a revision from before the reset is always refused.
 */
import { query } from "./db.ts";
import { validateCosts, type CostSheet } from "./costs.ts";
import { DEFAULT_COSTS } from "./defaultCosts.ts";

const KEY = "costs";

export interface StoredCosts {
  sheet: CostSheet;
  updatedAt: string | null;
  isDefault: boolean;
  /** Revision of the stored sheet; null while the built-in defaults are in use. */
  rev: number | null;
}

export async function loadCosts(): Promise<StoredCosts> {
  const rows = await query<{ value: unknown; updated_at: unknown; rev: unknown }>(
    `SELECT value, updated_at, rev FROM settings WHERE key = $1`,
    [KEY],
  );
  const row = rows[0];
  if (row) {
    const value = typeof row.value === "string" ? (JSON.parse(row.value) as unknown) : row.value;
    const rev = Number(row.rev);
    if (value === null) {
      // Reset to the defaults (see resetCosts).
      return { sheet: structuredClone(DEFAULT_COSTS), updatedAt: null, isDefault: true, rev: Number.isFinite(rev) ? rev : 0 };
    }
    // Stored by an older build or edited by hand: fall back to the defaults
    // rather than serving a sheet that no longer validates.
    const v = validateCosts(value);
    if (v.ok) {
      const d = row.updated_at instanceof Date ? row.updated_at : new Date(String(row.updated_at));
      return {
        sheet: v.sheet,
        updatedAt: Number.isNaN(d.getTime()) ? null : d.toISOString(),
        isDefault: false,
        rev: Number.isFinite(rev) ? rev : 0,
      };
    }
    console.error(`[costs] stored sheet is invalid (${v.error}); serving defaults`);
    // Keep the revision so the next save replaces the bad row instead of conflicting with it.
    return { sheet: structuredClone(DEFAULT_COSTS), updatedAt: null, isDefault: true, rev: Number.isFinite(rev) ? rev : 0 };
  }
  return { sheet: structuredClone(DEFAULT_COSTS), updatedAt: null, isDefault: true, rev: null };
}

/** False when the stored revision is not `expectedRev` (someone saved in between). */
export async function saveCosts(sheet: CostSheet, expectedRev: number | null, force = false): Promise<boolean> {
  const value = JSON.stringify(sheet);
  if (force) {
    await query(
      `INSERT INTO settings (key, value, updated_at, rev) VALUES ($1, $2::jsonb, now(), 1)
       ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, updated_at = now(), rev = settings.rev + 1`,
      [KEY, value],
    );
    return true;
  }
  const rows =
    expectedRev === null
      ? await query(
          `INSERT INTO settings (key, value, updated_at, rev) VALUES ($1, $2::jsonb, now(), 1)
           ON CONFLICT (key) DO NOTHING RETURNING rev`,
          [KEY, value],
        )
      : await query(
          `UPDATE settings SET value = $2::jsonb, updated_at = now(), rev = rev + 1
           WHERE key = $1 AND rev = $3 RETURNING rev`,
          [KEY, value, expectedRev],
        );
  return rows.length > 0;
}

/** Back to the built-in defaults. False on a revision conflict. */
export async function resetCosts(expectedRev: number | null, force = false): Promise<boolean> {
  if (force) {
    await query(`UPDATE settings SET value = 'null'::jsonb, updated_at = now(), rev = rev + 1 WHERE key = $1`, [KEY]);
    return true;
  }
  if (expectedRev === null) {
    // The editor saw the defaults with no row at all: fine only if there still is none.
    const rows = await query(`SELECT 1 FROM settings WHERE key = $1`, [KEY]);
    return rows.length === 0;
  }
  const rows = await query(
    `UPDATE settings SET value = 'null'::jsonb, updated_at = now(), rev = rev + 1
     WHERE key = $1 AND rev = $2 RETURNING rev`,
    [KEY, expectedRev],
  );
  return rows.length > 0;
}
