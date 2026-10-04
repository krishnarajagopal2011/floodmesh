/**
 * Everything a plan consists of, its defaults, and loading a saved plan file
 * (which may come from anywhere, so it is checked field by field).
 */
import type { PlacedUnit, PolygonRings } from "./plan.ts";
import { DEFAULT_MODEL, DEFAULT_PROFILES, ENVIRONMENTS, SENSITIVITY_DBM, type RadioModel, type UnitKind, type UnitProfile } from "./radio.ts";

export interface AreaState {
  name: string;
  /** constituency | locality | drawn | file */
  source: string;
  polygons: PolygonRings[];
  /** Assembly constituency numbers when picked from the list. */
  acNos?: number[];
}

export type DemandMode = "density" | "manual" | "osm";

export interface Demand {
  mode: DemandMode;
  householdsPerKm2: number;
  manualHouseholds: number;
  osmBuildings: number | null;
  householdsPerBuilding: number;
  /** Share of households that get a unit, %. */
  adoptionPct: number;
}

export interface PlanState {
  area: AreaState | null;
  units: PlacedUnit[];
  model: RadioModel;
  envId: string;
  profiles: Record<UnitKind, UnitProfile>;
  demand: Demand;
  redundancy: 1 | 2;
  /** Responder handsets beyond the placed responder units (for the cost). */
  extraResponders: number;
  report: ReportInfo;
}

export interface ReportInfo {
  title: string;
  preparedFor: string;
  preparedBy: string;
  notes: string;
}

export const DENSITY_PRESETS = [
  { label: "Dense core (≈7,000 households/km²)", value: 7000 },
  { label: "Urban (≈4,000 households/km²)", value: 4000 },
  { label: "Suburban (≈1,500 households/km²)", value: 1500 },
  { label: "Peri-urban / village (≈400 households/km²)", value: 400 },
];

export function defaultPlan(): PlanState {
  return {
    area: null,
    units: [],
    model: { ...DEFAULT_MODEL },
    envId: "urban",
    profiles: structuredClone(DEFAULT_PROFILES) as Record<UnitKind, UnitProfile>,
    demand: {
      mode: "density",
      householdsPerKm2: 4000,
      manualHouseholds: 1000,
      osmBuildings: null,
      householdsPerBuilding: 2,
      adoptionPct: 25,
    },
    redundancy: 2,
    extraResponders: 0,
    report: { title: "", preparedFor: "", preparedBy: "", notes: "" },
  };
}

/**
 * True when the household figure is waiting for an OSM building count (never
 * counted, or the area changed since). Households are then 0, never a hidden
 * density figure; the KPI, Cost tab and report say "count buildings first".
 */
export function demandPending(d: Demand): boolean {
  return d.mode === "osm" && d.osmBuildings === null;
}

export function households(d: Demand, areaKm2: number): number {
  if (d.mode === "manual") return Math.max(0, Math.round(d.manualHouseholds));
  if (d.mode === "osm") return d.osmBuildings === null ? 0 : Math.round(d.osmBuildings * d.householdsPerBuilding);
  return Math.round(areaKm2 * d.householdsPerKm2);
}

export function householdUnits(d: Demand, areaKm2: number): number {
  return Math.round((households(d, areaKm2) * d.adoptionPct) / 100);
}

// ----------------------------------------------------------- plan files

export const PLAN_APP = "floodmesh-coverage-planner";

export function toPlanFile(s: PlanState): object {
  return { app: PLAN_APP, version: 1, savedAt: new Date().toISOString(), ...s };
}

const num = (v: unknown, lo: number, hi: number, dflt: number) => {
  const n = typeof v === "number" ? v : Number(v);
  return Number.isFinite(n) ? Math.min(Math.max(n, lo), hi) : dflt;
};
const str = (v: unknown, max: number, dflt = "") => (typeof v === "string" ? v.slice(0, max) : dflt);

/** Above this many vertices an outline is thinned: every grid cell is tested against every vertex. */
export const MAX_AREA_VERTICES = 6000;

/**
 * Clean polygons from any source (plan files, GeoJSON imports, OSM search):
 * only numeric [lng, lat] pairs on Earth, rings of at least 4 points, at
 * most MAX_AREA_VERTICES vertices in all (evenly thinned beyond that).
 */
export function sanitizePolygons(v: unknown): PolygonRings[] {
  if (!Array.isArray(v)) return [];
  const out: PolygonRings[] = [];
  for (const poly of v.slice(0, 1000)) {
    if (!Array.isArray(poly)) continue;
    const clean = (ring: unknown): [number, number][] =>
      Array.isArray(ring)
        ? ring
            .filter(
              (p): p is [number, number] =>
                Array.isArray(p) &&
                typeof p[0] === "number" &&
                typeof p[1] === "number" &&
                Math.abs(p[0]) <= 180 &&
                Math.abs(p[1]) <= 90,
            )
            .map((p) => [p[0], p[1]] as [number, number])
        : [];
    const outer = clean(poly[0]);
    // A polygon whose outer ring is unusable is dropped with its holes.
    if (outer.length < 4) continue;
    out.push([outer, ...poly.slice(1, 200).map(clean).filter((r) => r.length >= 4)]);
  }
  const total = out.reduce((n, rings) => n + rings.reduce((m, r) => m + r.length, 0), 0);
  if (total <= MAX_AREA_VERTICES) return out;
  const step = Math.ceil(total / MAX_AREA_VERTICES);
  const thin = (r: [number, number][]) => {
    const kept = r.filter((_, i) => i % step === 0);
    kept.push(r[r.length - 1]); // keep the ring closed
    return kept;
  };
  const thinned: PolygonRings[] = [];
  for (const rings of out) {
    const outer = thin(rings[0]);
    if (outer.length < 4) continue; // too small to matter at this scale
    thinned.push([outer, ...rings.slice(1).map(thin).filter((r) => r.length >= 4)]);
  }
  return thinned;
}

/** Unit ids must be unique: markers, list rows and edits are keyed by them. */
function uniqueId(raw: string, i: number, used: Set<string>): string {
  let id = raw || `u${i}`;
  for (let k = 1; used.has(id); k++) id = `${raw || `u${i}`}_${k}`;
  used.add(id);
  return id;
}

function profile(v: unknown, d: UnitProfile): UnitProfile {
  const o = (v ?? {}) as Record<string, unknown>;
  return {
    heightM: num(o.heightM, 0.5, 200, d.heightM),
    antennaDbi: num(o.antennaDbi, -10, 20, d.antennaDbi),
    cableLossDb: num(o.cableLossDb, 0, 20, d.cableLossDb),
    indoorLossDb: num(o.indoorLossDb, 0, 60, d.indoorLossDb),
    txDbm: num(o.txDbm, 0, 30, d.txDbm),
  };
}

/** Read a plan file. Unknown or out-of-range values fall back to defaults. */
export function fromPlanFile(input: unknown): PlanState {
  const o = (input ?? {}) as Record<string, unknown>;
  if (o.app !== PLAN_APP) throw new Error("This is not a FloodMesh coverage plan file.");
  const d = defaultPlan();
  const a = o.area as Record<string, unknown> | null;
  const area: AreaState | null =
    a && typeof a === "object"
      ? {
          name: str(a.name, 200, "Area"),
          source: str(a.source, 20, "file"),
          polygons: sanitizePolygons(a.polygons),
          acNos: Array.isArray(a.acNos) ? a.acNos.filter((n) => Number.isInteger(n)).slice(0, 300) : undefined,
        }
      : null;
  const usedIds = new Set<string>();
  const units: PlacedUnit[] = Array.isArray(o.units)
    ? (o.units as Record<string, unknown>[])
        .filter((u) => u && (u.kind === "powered" || u.kind === "responder"))
        .slice(0, 5000)
        .map((u, i) => ({
          id: uniqueId(str(u.id, 40), i, usedIds),
          kind: u.kind as PlacedUnit["kind"],
          lat: num(u.lat, -90, 90, 0),
          lng: num(u.lng, -180, 180, 0),
          name: str(u.name, 40, `U${i + 1}`).replace(/[\u0000-\u001f\u007f]/g, " "),
          heightM: u.heightM === undefined ? undefined : num(u.heightM, 0.5, 200, 10),
          antennaDbi: u.antennaDbi === undefined ? undefined : num(u.antennaDbi, -10, 20, 2.2),
        }))
    : [];
  const m = (o.model ?? {}) as Record<string, unknown>;
  const sf = num(m.sf, 7, 12, d.model.sf);
  const model: RadioModel = {
    sf: Object.hasOwn(SENSITIVITY_DBM, Math.round(sf)) ? Math.round(sf) : d.model.sf,
    pathLossExponent: num(m.pathLossExponent, 2, 6, d.model.pathLossExponent),
    clutterDb: num(m.clutterDb, -20, 40, d.model.clutterDb),
    fadeMarginDb: num(m.fadeMarginDb, 0, 40, d.model.fadeMarginDb),
    heightGainCapDb: num(m.heightGainCapDb, 0, 40, d.model.heightGainCapDb),
    hopLimit: Math.round(num(m.hopLimit, 1, 7, d.model.hopLimit)),
    wakePreambleS: num(m.wakePreambleS, 0, 5, d.model.wakePreambleS),
    dutyCyclePct: num(m.dutyCyclePct, 0.1, 100, d.model.dutyCyclePct),
    maxErpDbm: num(m.maxErpDbm, 0, 40, d.model.maxErpDbm),
    heartbeatsPerHour: num(m.heartbeatsPerHour, 0, 60, d.model.heartbeatsPerHour),
  };
  const pr = (o.profiles ?? {}) as Record<string, unknown>;
  const profiles = {
    civilian: profile(pr.civilian, d.profiles.civilian),
    powered: profile(pr.powered, d.profiles.powered),
    responder: profile(pr.responder, d.profiles.responder),
  };
  const dm = (o.demand ?? {}) as Record<string, unknown>;
  const mode = dm.mode === "manual" || dm.mode === "osm" ? dm.mode : "density";
  const rep = (o.report ?? {}) as Record<string, unknown>;
  const envId = ENVIRONMENTS.some((e) => e.id === o.envId) ? String(o.envId) : "custom";
  return {
    area: area && area.polygons.length ? area : null,
    units,
    model,
    envId,
    profiles,
    demand: {
      mode,
      householdsPerKm2: num(dm.householdsPerKm2, 0, 100_000, d.demand.householdsPerKm2),
      manualHouseholds: num(dm.manualHouseholds, 0, 10_000_000, d.demand.manualHouseholds),
      osmBuildings: dm.osmBuildings === null || dm.osmBuildings === undefined ? null : num(dm.osmBuildings, 0, 10_000_000, 0),
      householdsPerBuilding: num(dm.householdsPerBuilding, 0, 1000, d.demand.householdsPerBuilding),
      adoptionPct: num(dm.adoptionPct, 0, 100, d.demand.adoptionPct),
    },
    redundancy: o.redundancy === 1 ? 1 : 2,
    extraResponders: Math.round(num(o.extraResponders, 0, 100_000, 0)),
    report: {
      title: str(rep.title, 200),
      preparedFor: str(rep.preparedFor, 200),
      preparedBy: str(rep.preparedBy, 200),
      notes: str(rep.notes, 4000),
    },
  };
}
