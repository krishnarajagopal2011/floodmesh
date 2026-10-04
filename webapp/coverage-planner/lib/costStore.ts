/** Reads and writes the cost sheet in the settings table. Server only. */
import { query } from "./db.ts";
import { DEFAULT_COSTS, validateCosts, type CostSheet } from "./costs.ts";

const KEY = "costs";

export interface StoredCosts {
  sheet: CostSheet;
  updatedAt: string | null;
  isDefault: boolean;
}

export async function loadCosts(): Promise<StoredCosts> {
  const rows = await query<{ value: unknown; updated_at: unknown }>(
    `SELECT value, updated_at FROM settings WHERE key = $1`,
    [KEY],
  );
  const row = rows[0];
  if (row) {
    // Stored by an older build or edited by hand: fall back to the defaults
    // rather than serving a sheet that no longer validates.
    const value = typeof row.value === "string" ? (JSON.parse(row.value) as unknown) : row.value;
    const v = validateCosts(value);
    if (v.ok) {
      const d = row.updated_at instanceof Date ? row.updated_at : new Date(String(row.updated_at));
      return { sheet: v.sheet, updatedAt: Number.isNaN(d.getTime()) ? null : d.toISOString(), isDefault: false };
    }
    console.error(`[costs] stored sheet is invalid (${v.error}); serving defaults`);
  }
  return { sheet: structuredClone(DEFAULT_COSTS), updatedAt: null, isDefault: true };
}

export async function saveCosts(sheet: CostSheet): Promise<void> {
  await query(
    `INSERT INTO settings (key, value, updated_at) VALUES ($1, $2::jsonb, now())
     ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, updated_at = now()`,
    [KEY, JSON.stringify(sheet)],
  );
}

export async function resetCosts(): Promise<void> {
  await query(`DELETE FROM settings WHERE key = $1`, [KEY]);
}
