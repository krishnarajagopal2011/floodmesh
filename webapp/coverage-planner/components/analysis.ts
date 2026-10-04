/**
 * Runs the radio model over a plan and turns the result into what the map
 * and the panels show. Browser only (the coverage image uses a canvas).
 */
import {
  analyseBackbone,
  analyseCoverage,
  airtimeReport,
  makeProjection,
  gridCellSize,
  projectArea,
  buildGrid,
  unitProfile,
  type AirtimeReport,
  type Backbone,
  type Coverage,
  type Grid,
  type LngLat,
  type PlacedUnit,
  type ProjectedArea,
  type Projection,
} from "@/lib/plan";
import { marginDb, rangeM, rssiDbm, UNIT_LABELS, type RadioModel, type UnitKind, type UnitProfile } from "@/lib/radio";
import { analyseHouseholds, type HouseholdAnalysis } from "@/lib/households";
import type { PlanState } from "@/lib/planState";
import type { LinkLine, UnitLabel } from "./mapController";

export type CoverageView = "redundancy" | "signal" | "sos" | "none";

export interface Ranges {
  householdPowered: number;
  poweredPowered: number;
  householdResponder: number;
  responderPowered: number;
  householdHousehold: number;
}

export interface Analysis {
  proj: Projection;
  area: ProjectedArea | null;
  grid: Grid | null;
  coverage: Coverage | null;
  backbone: Backbone;
  airtime: AirtimeReport;
  ranges: Ranges;
  areaKm2: number;
  labels: Map<string, UnitLabel>;
  links: LinkLine[];
  /** Islands that contain at least one powered unit. */
  poweredIslands: number;
  /** Most relays any powered unit needs to reach a responder (−1 = none reachable). */
  maxRelays: number;
  poweredUnreachable: number;
  /** Placed household units and what each can reach, when any are placed. */
  households: HouseholdAnalysis | null;
  ms: number;
}

export function computeRanges(p: Record<UnitKind, UnitProfile>, m: RadioModel): Ranges {
  return {
    householdPowered: rangeM(p.civilian, p.powered, m),
    poweredPowered: rangeM(p.powered, p.powered, m),
    householdResponder: rangeM(p.civilian, p.responder, m),
    responderPowered: rangeM(p.responder, p.powered, m),
    householdHousehold: rangeM(p.civilian, p.civilian, m),
  };
}

function centre(plan: PlanState): [number, number] {
  let sx = 0;
  let sy = 0;
  let n = 0;
  for (const poly of plan.area?.polygons ?? []) {
    for (const [x, y] of poly[0] ?? []) {
      sx += x;
      sy += y;
      n++;
    }
  }
  if (!n) for (const u of plan.units) {
    sx += u.lng;
    sy += u.lat;
    n++;
  }
  return n ? [sy / n, sx / n] : [13.05, 80.21];
}

export function analyse(plan: PlanState): Analysis {
  const t0 = performance.now();
  const [lat0, lng0] = centre(plan);
  const proj = makeProjection(lat0, lng0);
  const { units, profiles, model } = plan;
  const backbone = analyseBackbone(units, profiles, model, proj);
  let area: ProjectedArea | null = null;
  let grid: Grid | null = null;
  let coverage: Coverage | null = null;
  let areaKm2 = 0;
  if (plan.area && plan.area.polygons.length) {
    area = projectArea(plan.area.polygons, proj);
    grid = buildGrid(area, gridCellSize(area));
    areaKm2 = (grid.inside.length * grid.cellM * grid.cellM) / 1e6;
    coverage = analyseCoverage(area, grid, units, profiles, model, proj, backbone);
  }

  const labels = new Map<string, UnitLabel>();
  for (const u of units) labels.set(u.id, { id: u.id, rangeM: rangeM(profiles.civilian, unitProfile(u, profiles), model) });

  const links: LinkLine[] = backbone.treeEdges.map((k) => {
    const e = backbone.edges[k];
    return { a: [units[e.a].lat, units[e.a].lng], b: [units[e.b].lat, units[e.b].lng], marginDb: e.marginDb };
  });

  const islands = new Set<number>();
  let maxRelays = -1;
  let poweredUnreachable = 0;
  units.forEach((u, i) => {
    if (u.kind !== "powered") return;
    islands.add(backbone.component[i]);
    const r = backbone.relaysToResponder[i];
    if (r < 0 || r > model.hopLimit) poweredUnreachable++;
    if (r > maxRelays) maxRelays = r;
  });

  const households = plan.householdPoints?.points.length
    ? analyseHouseholds(plan.householdPoints.points, units, profiles, model, proj, backbone)
    : null;

  return {
    proj,
    area,
    grid,
    coverage,
    households,
    backbone,
    airtime: airtimeReport(units, backbone, model),
    ranges: computeRanges(profiles, model),
    areaKm2,
    labels,
    links,
    poweredIslands: islands.size,
    maxRelays,
    poweredUnreachable,
    ms: performance.now() - t0,
  };
}

// ------------------------------------------------------------------ image

type RGB = [number, number, number];
export const PALETTE = {
  none: [200, 30, 30] as RGB,
  one: [235, 170, 0] as RGB,
  two: [20, 150, 70] as RGB,
};

function marginColour(m: number): RGB {
  if (!Number.isFinite(m) || m < 0) return [200, 30, 30];
  if (m < 5) return [240, 120, 20];
  if (m < 10) return [235, 190, 0];
  if (m < 20) return [110, 190, 70];
  return [20, 130, 60];
}

function relaysColour(r: number, hopLimit: number): RGB {
  if (r < 0) return [200, 30, 30];
  if (r === 0) return [20, 110, 50];
  // At the limit before "1 relay": with a hop limit of 1, one relay is the limit.
  if (r >= hopLimit) return [235, 170, 0];
  if (r === 1) return [20, 150, 70];
  return [130, 190, 60];
}

const css = ([r, g, b]: RGB) => `rgb(${r},${g},${b})`;

/** Colour of each placed household unit for the current map layer (same keys as the coverage image). */
export function householdColours(a: Analysis, view: CoverageView, hopLimit: number): string[] {
  const h = a.households;
  if (!h) return [];
  const out = new Array<string>(h.heard.length);
  for (let i = 0; i < h.heard.length; i++) {
    if (view === "sos") out[i] = css(relaysColour(h.relays[i], hopLimit));
    else if (view === "none") out[i] = "rgb(55,65,81)";
    else out[i] = css(h.heard[i] >= 2 ? PALETTE.two : h.heard[i] === 1 ? PALETTE.one : PALETTE.none);
  }
  return out;
}

export interface CoverageImage {
  url: string;
  sw: LngLat;
  ne: LngLat;
}

export function renderCoverage(a: Analysis, view: CoverageView, hopLimit: number): CoverageImage | null {
  const { grid, coverage, proj } = a;
  if (!grid || !coverage || view === "none") return null;
  const canvas = document.createElement("canvas");
  canvas.width = grid.cols;
  canvas.height = grid.rows;
  const ctx = canvas.getContext("2d");
  if (!ctx) return null;
  const img = ctx.createImageData(grid.cols, grid.rows);
  for (let k = 0; k < grid.inside.length; k++) {
    const idx = grid.inside[k];
    const r = Math.floor(idx / grid.cols);
    const c = idx - r * grid.cols;
    const p = ((grid.rows - 1 - r) * grid.cols + c) * 4;
    const col =
      view === "redundancy"
        ? coverage.count[k] >= 2
          ? PALETTE.two
          : coverage.count[k] === 1
            ? PALETTE.one
            : PALETTE.none
        : view === "signal"
          ? marginColour(coverage.bestMarginDb[k])
          : relaysColour(coverage.relays[k], hopLimit);
    img.data[p] = col[0];
    img.data[p + 1] = col[1];
    img.data[p + 2] = col[2];
    img.data[p + 3] = 255;
  }
  ctx.putImageData(img, 0, 0);
  return {
    url: canvas.toDataURL("image/png"),
    sw: proj.toLngLat(grid.originX, grid.originY),
    ne: proj.toLngLat(grid.originX + grid.cols * grid.cellM, grid.originY + grid.rows * grid.cellM),
  };
}

// ------------------------------------------------------------------ probe

export interface ProbeRow {
  unit: PlacedUnit;
  distanceM: number;
  rssiDbm: number;
  marginDb: number;
  relays: number;
}

/** Rows shown in the probe popup (and links drawn); its counts use every unit. */
export const PROBE_ROWS = 6;

/** What a household unit at this point would hear: every placed unit, strongest first. */
export function probe(plan: PlanState, a: Analysis, lat: number, lng: number): ProbeRow[] {
  const [x, y] = a.proj.toXY(lng, lat);
  const civ = plan.profiles.civilian;
  return plan.units
    .map((u, i) => {
      const [ux, uy] = a.proj.toXY(u.lng, u.lat);
      const d = Math.hypot(ux - x, uy - y);
      const p = unitProfile(u, plan.profiles);
      return {
        unit: u,
        distanceM: d,
        rssiDbm: Math.min(rssiDbm(d, p, civ, plan.model), rssiDbm(d, civ, p, plan.model)),
        marginDb: marginDb(d, civ, p, plan.model),
        relays: u.kind === "responder" ? 0 : a.backbone.relaysToResponder[i],
      };
    })
    .sort((p, q) => q.marginDb - p.marginDb);
}

function esc(s: string): string {
  return s.replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
}

export function probeHtml(rows: ProbeRow[], plan: PlanState): string {
  const ok = rows.filter((r) => r.marginDb >= 0 && r.unit.kind === "powered").length;
  const direct = rows.some((r) => r.marginDb >= 0 && r.unit.kind === "responder");
  const relays = rows
    .filter((r) => r.marginDb >= 0 && r.unit.kind === "powered" && r.relays > 0)
    .reduce((m, r) => Math.min(m, r.relays), Infinity);
  const sos = direct
    ? "reaches a responder directly"
    : Number.isFinite(relays) && relays <= plan.model.hopLimit
      ? `reaches a responder through ${relays} relay${relays === 1 ? "" : "s"}`
      : "does not reach a responder within the hop limit";
  const shown = rows.slice(0, PROBE_ROWS);
  const body = shown
    .map(
      (r) =>
        `<tr class="${r.marginDb >= 0 ? "ok" : "bad"}"><td>${esc(r.unit.name)}</td><td>${esc(UNIT_LABELS[r.unit.kind].split(" ")[0])}</td>` +
        `<td>${r.distanceM < 1000 ? Math.round(r.distanceM) + " m" : (r.distanceM / 1000).toFixed(2) + " km"}</td>` +
        `<td>${r.rssiDbm.toFixed(0)} dBm</td><td>${r.marginDb >= 0 ? "+" : ""}${r.marginDb.toFixed(1)} dB</td></tr>`,
    )
    .join("");
  return (
    `<div class="probe"><strong>Household unit here (indoors, ${plan.profiles.civilian.heightM} m)</strong>` +
    `<p>${ok} powered unit${ok === 1 ? "" : "s"} in range; SOS ${sos}.</p>` +
    (rows.length
      ? `<table><thead><tr><th>Unit</th><th>Type</th><th>Distance</th><th>Signal</th><th>Margin</th></tr></thead><tbody>${body}</tbody></table>` +
        (rows.length > shown.length ? `<p class="small">Strongest ${shown.length} of ${rows.length} placed units shown.</p>` : "") +
        `<p class="small">Signal: weaker direction of the link. Margin: left over after the ${plan.model.fadeMarginDb} dB fade margin; ≥ 0 means the link is expected to work.</p>`
      : "<p>No units placed yet.</p>") +
    `</div>`
  );
}
