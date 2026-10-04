/**
 * The cost sheet: what each kind of unit and each project task costs.
 *
 * The admin edits the full sheet on /admin (component lines, markup). The
 * planner page only ever receives the public view: one price per unit kind
 * and one rate per project line, markup included, no breakdown.
 *
 * The built-in default sheet lives in defaultCosts.ts, which only server code
 * imports, so its breakdown is not shipped in the public JavaScript.
 */
import type { UnitKind } from "./radio.ts";

export const COST_BASES = {
  fixed: "Fixed, once",
  per_km2: "Per km² of area",
  per_household_unit: "Per household unit",
  per_powered_unit: "Per powered unit",
  per_responder_unit: "Per responder unit",
} as const;
export type CostBasis = keyof typeof COST_BASES;

export interface CostLine {
  id: string;
  name: string;
  amount: number;
  note: string;
}

export interface ProjectLine extends CostLine {
  basis: CostBasis;
  /**
   * Add the markup to this line's public rate. Off for pass-through fees whose
   * real cost is public (WPC approval): marking those up would let anyone work
   * out the markup by dividing the rate by the known fee.
   */
  markup: boolean;
}

export interface CostSheet {
  version: 1;
  currency: "INR";
  /** Added to every amount in the public view, %. */
  markupPct: number;
  /** Added to the subtotal in the planner, %. Shown publicly. */
  contingencyPct: number;
  units: Record<UnitKind, CostLine[]>;
  project: ProjectLine[];
}

export interface PublicCosts {
  currency: "INR";
  contingencyPct: number;
  unitPrice: Record<UnitKind, number>;
  project: { id: string; name: string; basis: CostBasis; rate: number }[];
  updatedAt: string | null;
  isDefault: boolean;
}

const UNIT_KINDS: UnitKind[] = ["civilian", "powered", "responder"];

// ------------------------------------------------------------- validation

const MAX_LINES = 40;
const MAX_AMOUNT = 1e9;

function text(v: unknown, max: number): string {
  return typeof v === "string" ? v.replace(/[\u0000-\u001f\u007f]/g, " ").trim().slice(0, max) : "";
}

/** Numbers and non-empty numeric strings only: null, "", true and [] are not 0 or 1. */
function strictNumber(v: unknown): number {
  if (typeof v === "number") return v;
  if (typeof v === "string" && v.trim() !== "") return Number(v);
  return NaN;
}

function amount(v: unknown): number | null {
  const n = strictNumber(v);
  if (!Number.isFinite(n) || n < 0 || n > MAX_AMOUNT) return null;
  return Math.round(n * 100) / 100;
}

function pct(v: unknown): number | null {
  const n = strictNumber(v);
  return Number.isFinite(n) && n >= 0 && n <= 500 ? Math.round(n * 100) / 100 : null;
}

/** A clean id, unique within its list (React keys and edits rely on it). */
function lineId(v: unknown, i: number, used: Set<string>): string {
  const base = text(v, 40).replace(/[^a-zA-Z0-9_-]/g, "") || `line${i + 1}`;
  let id = base;
  for (let k = 2; used.has(id); k++) id = `${base}-${k}`;
  used.add(id);
  return id;
}

/** Check an untrusted cost sheet. Returns the clean sheet or the first problem found. */
export function validateCosts(input: unknown): { ok: true; sheet: CostSheet } | { ok: false; error: string } {
  if (!input || typeof input !== "object") return { ok: false, error: "Cost sheet must be an object." };
  const o = input as Record<string, unknown>;
  const markupPct = pct(o.markupPct);
  const contingencyPct = pct(o.contingencyPct);
  if (markupPct === null) return { ok: false, error: "Markup must be between 0 and 500%." };
  if (contingencyPct === null) return { ok: false, error: "Contingency must be between 0 and 500%." };

  const lines = (v: unknown, where: string): CostLine[] | string => {
    if (!Array.isArray(v)) return `${where}: list of lines expected.`;
    if (v.length > MAX_LINES) return `${where}: at most ${MAX_LINES} lines.`;
    const out: CostLine[] = [];
    const used = new Set<string>();
    for (let i = 0; i < v.length; i++) {
      const l = (v[i] ?? {}) as Record<string, unknown>;
      const name = text(l.name, 120);
      const a = amount(l.amount);
      if (!name) return `${where}, line ${i + 1}: name is empty.`;
      if (a === null) return `${where}, "${name}": amount must be a number from 0 to ${MAX_AMOUNT}.`;
      out.push({ id: lineId(l.id, i, used), name, amount: a, note: text(l.note, 200) });
    }
    return out;
  };

  const unitsIn = (o.units ?? {}) as Record<string, unknown>;
  const units = {} as Record<UnitKind, CostLine[]>;
  for (const k of UNIT_KINDS) {
    const r = lines(unitsIn[k], `Unit "${k}"`);
    if (typeof r === "string") return { ok: false, error: r };
    units[k] = r;
  }

  const projIn = lines(o.project, "Project costs");
  if (typeof projIn === "string") return { ok: false, error: projIn };
  const rawProject = o.project as Record<string, unknown>[];
  const project: ProjectLine[] = [];
  for (let i = 0; i < projIn.length; i++) {
    const basis = rawProject[i]?.basis;
    // Object.hasOwn, not `in`: "toString" or "__proto__" must not pass.
    if (typeof basis !== "string" || !Object.hasOwn(COST_BASES, basis)) {
      return { ok: false, error: `Project costs, "${projIn[i].name}": unknown basis.` };
    }
    // Sheets saved before the flag existed marked every line up.
    const markup = rawProject[i]?.markup;
    project.push({ ...projIn[i], basis: basis as CostBasis, markup: typeof markup === "boolean" ? markup : true });
  }

  return { ok: true, sheet: { version: 1, currency: "INR", markupPct, contingencyPct, units, project } };
}

// ------------------------------------------------------------- public view

export function sumLines(lines: CostLine[]): number {
  return lines.reduce((s, l) => s + l.amount, 0);
}

const round = (v: number) => Math.round(v * 100) / 100;

export function publicCosts(sheet: CostSheet, updatedAt: string | null, isDefault: boolean): PublicCosts {
  const k = 1 + sheet.markupPct / 100;
  return {
    currency: sheet.currency,
    contingencyPct: sheet.contingencyPct,
    unitPrice: {
      civilian: round(sumLines(sheet.units.civilian) * k),
      powered: round(sumLines(sheet.units.powered) * k),
      responder: round(sumLines(sheet.units.responder) * k),
    },
    project: sheet.project.map((l) => ({ id: l.id, name: l.name, basis: l.basis, rate: round(l.amount * (l.markup ? k : 1)) })),
    updatedAt,
    isDefault,
  };
}

// ------------------------------------------------------------------ totals

export interface Quantities {
  civilian: number;
  powered: number;
  responder: number;
  areaKm2: number;
}

export interface EstimateLine {
  name: string;
  quantity: number;
  unit: string;
  rate: number;
  total: number;
}

export interface Estimate {
  lines: EstimateLine[];
  subtotal: number;
  contingency: number;
  total: number;
}

export function estimate(c: PublicCosts, q: Quantities): Estimate {
  const lines: EstimateLine[] = [
    { name: "Household units", quantity: q.civilian, unit: "units", rate: c.unitPrice.civilian, total: 0 },
    { name: "Powered units (terrace)", quantity: q.powered, unit: "units", rate: c.unitPrice.powered, total: 0 },
    { name: "Responder units", quantity: q.responder, unit: "units", rate: c.unitPrice.responder, total: 0 },
  ];
  for (const p of c.project) {
    const quantity =
      p.basis === "fixed"
        ? 1
        : p.basis === "per_km2"
          ? round(q.areaKm2)
          : p.basis === "per_household_unit"
            ? q.civilian
            : p.basis === "per_powered_unit"
              ? q.powered
              : q.responder;
    const unit = p.basis === "fixed" ? "lump sum" : p.basis === "per_km2" ? "km²" : "units";
    lines.push({ name: p.name, quantity, unit, rate: p.rate, total: 0 });
  }
  for (const l of lines) l.total = round(l.quantity * l.rate);
  const subtotal = round(lines.reduce((s, l) => s + l.total, 0));
  const contingency = round((subtotal * c.contingencyPct) / 100);
  return { lines, subtotal, contingency, total: round(subtotal + contingency) };
}

export function formatInr(v: number): string {
  return "₹" + Math.round(v).toLocaleString("en-IN");
}
