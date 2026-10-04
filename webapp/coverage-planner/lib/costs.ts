/**
 * The cost sheet: what each kind of unit and each project task costs.
 *
 * The admin edits the full sheet on /admin (component lines, markup). The
 * planner page only ever receives the public view: one price per unit kind
 * and one rate per project line, markup included, no breakdown.
 *
 * Default amounts: the electronics line is the landed cost per unit from the
 * priced PCB V1 BoM (docs/hardware/pcb-v1/flood-mesh-v1.0-bom-priced.xlsx,
 * 15-unit build, 27 Sep 2026), the whip antenna is the §14.1 candidate part,
 * WPC type approval is from docs/certification-india.md. Every other default
 * is a placeholder estimate to be replaced with quotes.
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

const base = (): CostLine[] => [
  {
    id: "electronics",
    name: "Electronics, battery and case parts (PCB V1 priced BoM, landed)",
    amount: 2417,
    note: "BoM: 15-unit build, 27 Sep 2026",
  },
  { id: "pcb", name: "PCB fabrication and assembly", amount: 450, note: "Estimate" },
  { id: "enclosure", name: "IP67 enclosure", amount: 450, note: "Estimate" },
  { id: "pigtail", name: "U.FL to SMA bulkhead pigtail", amount: 150, note: "Estimate" },
  { id: "keypad", name: "4×4 membrane keypad", amount: 60, note: "Estimate" },
  { id: "label", name: "Label with call sign and QR code, packaging", amount: 40, note: "Estimate" },
];

export const DEFAULT_COSTS: CostSheet = {
  version: 1,
  currency: "INR",
  markupPct: 0,
  contingencyPct: 10,
  units: {
    civilian: [
      ...base(),
      { id: "whip", name: "868 MHz half-wave whip antenna (LWC-868-RD-RA-SMA)", amount: 307, note: "§14.1 candidate" },
    ],
    powered: [
      ...base(),
      { id: "omni", name: "Outdoor omni antenna, 5–6 dBi fibreglass", amount: 2500, note: "Estimate" },
      { id: "coax", name: "Low-loss coax (LMR-240, 2 m) and connectors", amount: 600, note: "Estimate" },
      { id: "surge", name: "Surge arrestor and earthing", amount: 1200, note: "Estimate" },
      { id: "solar", name: "Solar panel 20 W, charge controller and backup battery", amount: 3500, note: "Estimate" },
      { id: "mount", name: "Pole or wall mount", amount: 800, note: "Estimate" },
      { id: "install", name: "Terrace installation labour", amount: 1500, note: "Estimate" },
    ],
    responder: [
      ...base(),
      { id: "whip", name: "868 MHz half-wave whip antenna (LWC-868-RD-RA-SMA)", amount: 307, note: "§14.1 candidate" },
      { id: "powerbank", name: "Power bank, 10,000 mAh", amount: 900, note: "Estimate" },
      { id: "pouch", name: "Waterproof pouch and lanyard", amount: 250, note: "Estimate" },
    ],
  },
  project: [
    { id: "survey", name: "Site survey and walk test", basis: "per_km2", amount: 3000, note: "Estimate" },
    {
      id: "calibration",
      name: "Field calibration test (2 days, team of 4)",
      basis: "fixed",
      amount: 20000,
      note: "Estimate",
    },
    {
      id: "wpc",
      name: "WPC equipment type approval (per hardware model)",
      basis: "fixed",
      amount: 10000,
      note: "docs/certification-india.md",
    },
    {
      id: "handover",
      name: "Household handover and registry entry",
      basis: "per_household_unit",
      amount: 50,
      note: "Estimate",
    },
    { id: "training", name: "Responder training", basis: "per_responder_unit", amount: 1500, note: "Estimate" },
    {
      id: "maintenance",
      name: "Year-1 maintenance and battery swaps",
      basis: "per_powered_unit",
      amount: 600,
      note: "Estimate",
    },
  ],
};

// ------------------------------------------------------------- validation

const MAX_LINES = 40;
const MAX_AMOUNT = 1e9;

function text(v: unknown, max: number): string {
  return typeof v === "string" ? v.replace(/[\u0000-\u001f\u007f]/g, " ").trim().slice(0, max) : "";
}

function amount(v: unknown): number | null {
  const n = typeof v === "number" ? v : Number(v);
  if (!Number.isFinite(n) || n < 0 || n > MAX_AMOUNT) return null;
  return Math.round(n * 100) / 100;
}

function pct(v: unknown): number | null {
  const n = typeof v === "number" ? v : Number(v);
  return Number.isFinite(n) && n >= 0 && n <= 500 ? Math.round(n * 100) / 100 : null;
}

function lineId(v: unknown, i: number): string {
  const s = text(v, 40).replace(/[^a-zA-Z0-9_-]/g, "");
  return s || `line${i + 1}`;
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
    for (let i = 0; i < v.length; i++) {
      const l = (v[i] ?? {}) as Record<string, unknown>;
      const name = text(l.name, 120);
      const a = amount(l.amount);
      if (!name) return `${where}, line ${i + 1}: name is empty.`;
      if (a === null) return `${where}, "${name}": amount must be a number from 0 to ${MAX_AMOUNT}.`;
      out.push({ id: lineId(l.id, i), name, amount: a, note: text(l.note, 200) });
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
    if (typeof basis !== "string" || !(basis in COST_BASES)) {
      return { ok: false, error: `Project costs, "${projIn[i].name}": unknown basis.` };
    }
    project.push({ ...projIn[i], basis: basis as CostBasis });
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
    project: sheet.project.map((l) => ({ id: l.id, name: l.name, basis: l.basis, rate: round(l.amount * k) })),
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
