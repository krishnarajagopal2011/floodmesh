/**
 * Coverage analysis and automatic placement on a planar grid.
 *
 * Coordinates are projected to metres around the area's centre
 * (equirectangular), which is accurate to well under 1% over a constituency.
 * Pure functions, shared by the browser and the tests.
 */
import {
  DEFAULT_PROFILES,
  FSPL_1M_DB,
  frameAirtimeS,
  FRAME_BYTES,
  heightGainDb,
  maxPathLossDb,
  rangeM,
  type RadioModel,
  type UnitKind,
  type UnitProfile,
} from "./radio.ts";

export type LngLat = [number, number];
/** GeoJSON polygon rings: outer ring first, then holes. */
export type PolygonRings = LngLat[][];

export interface Projection {
  lat0: number;
  lng0: number;
  toXY(lng: number, lat: number): [number, number];
  toLngLat(x: number, y: number): LngLat;
}

export function makeProjection(lat0: number, lng0: number): Projection {
  const ky = 110_540;
  const kx = 111_320 * Math.cos((lat0 * Math.PI) / 180);
  return {
    lat0,
    lng0,
    toXY: (lng, lat) => [(lng - lng0) * kx, (lat - lat0) * ky],
    toLngLat: (x, y) => [lng0 + x / kx, lat0 + y / ky],
  };
}

/** A placed unit. Height and antenna may differ from the kind's profile. */
export interface PlacedUnit {
  id: string;
  kind: Exclude<UnitKind, "civilian">;
  lat: number;
  lng: number;
  name: string;
  heightM?: number;
  antennaDbi?: number;
}

export function unitProfile(u: PlacedUnit, profiles: Record<UnitKind, UnitProfile>): UnitProfile {
  const p = profiles[u.kind] ?? DEFAULT_PROFILES[u.kind];
  return {
    ...p,
    heightM: u.heightM ?? p.heightM,
    antennaDbi: u.antennaDbi ?? p.antennaDbi,
  };
}

// ------------------------------------------------------------------ polygons

type XYRing = Float64Array; // x0,y0,x1,y1,...

export interface ProjectedArea {
  /** Each polygon is a list of rings (outer first). */
  polygons: XYRing[][];
  minX: number;
  minY: number;
  maxX: number;
  maxY: number;
  areaM2: number;
}

function ringArea(r: XYRing): number {
  let a = 0;
  const n = r.length / 2;
  for (let i = 0, j = n - 1; i < n; j = i++) a += r[2 * j] * r[2 * i + 1] - r[2 * i] * r[2 * j + 1];
  return a / 2;
}

export function projectArea(polys: PolygonRings[], proj: Projection): ProjectedArea {
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  let areaM2 = 0;
  const polygons = polys.map((rings) =>
    rings.map((ring, k) => {
      const out = new Float64Array(ring.length * 2);
      ring.forEach(([lng, lat], i) => {
        const [x, y] = proj.toXY(lng, lat);
        out[2 * i] = x;
        out[2 * i + 1] = y;
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
      });
      areaM2 += (k === 0 ? 1 : -1) * Math.abs(ringArea(out));
      return out;
    }),
  );
  return { polygons, minX, minY, maxX, maxY, areaM2 };
}

function inRing(r: XYRing, x: number, y: number): boolean {
  let inside = false;
  const n = r.length / 2;
  for (let i = 0, j = n - 1; i < n; j = i++) {
    const xi = r[2 * i];
    const yi = r[2 * i + 1];
    const xj = r[2 * j];
    const yj = r[2 * j + 1];
    if (yi > y !== yj > y && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
}

export function insideArea(a: ProjectedArea, x: number, y: number): boolean {
  if (x < a.minX || x > a.maxX || y < a.minY || y > a.maxY) return false;
  for (const rings of a.polygons) {
    if (!inRing(rings[0], x, y)) continue;
    let hole = false;
    for (let k = 1; k < rings.length; k++) if (inRing(rings[k], x, y)) hole = true;
    if (!hole) return true;
  }
  return false;
}

// ---------------------------------------------------------------------- grid

export interface Grid {
  cellM: number;
  cols: number;
  rows: number;
  originX: number;
  originY: number;
  /** Indices (row * cols + col) of the cells whose centre is inside the area. */
  inside: Int32Array;
}

/** Cell size that keeps the grid near `maxCells` cells, rounded to a tidy number. */
export function pickCellSize(areaM2: number, maxCells = 40_000): number {
  const raw = Math.sqrt(Math.max(areaM2, 1) / maxCells);
  for (const s of [10, 15, 20, 25, 30, 40, 50, 75, 100, 150, 200, 250, 300, 400, 500]) if (s >= raw) return s;
  return Math.ceil(raw / 100) * 100;
}

export function buildGrid(a: ProjectedArea, cellM: number): Grid {
  const cols = Math.max(1, Math.ceil((a.maxX - a.minX) / cellM));
  const rows = Math.max(1, Math.ceil((a.maxY - a.minY) / cellM));
  const inside: number[] = [];
  for (let r = 0; r < rows; r++) {
    const y = a.minY + (r + 0.5) * cellM;
    for (let c = 0; c < cols; c++) {
      const x = a.minX + (c + 0.5) * cellM;
      if (insideArea(a, x, y)) inside.push(r * cols + c);
    }
  }
  return { cellM, cols, rows, originX: a.minX, originY: a.minY, inside: Int32Array.from(inside) };
}

export function cellXY(g: Grid, idx: number): [number, number] {
  const r = Math.floor(idx / g.cols);
  const c = idx - r * g.cols;
  return [g.originX + (c + 0.5) * g.cellM, g.originY + (r + 0.5) * g.cellM];
}

// -------------------------------------------------------------------- links

interface Node {
  unit: PlacedUnit;
  profile: UnitProfile;
  x: number;
  y: number;
}

export interface BackboneEdge {
  a: number;
  b: number;
  distanceM: number;
  marginDb: number;
}

export interface Backbone {
  edges: BackboneEdge[];
  /** Connected component id per unit (units = powered and responders). */
  component: Int32Array;
  components: number;
  /** For each unit: relays an SOS needs from it to a responder (powered: ≥ 1, responder: 0, unreachable: -1). */
  relaysToResponder: Int32Array;
  /** Powered units each powered unit would relay heartbeats for (within the hop limit). */
  heartbeatSources: Int32Array;
  /** Links worth drawing: a minimum spanning tree of `edges` (indices). */
  treeEdges: number[];
}

function linkMarginDb(d: number, a: UnitProfile, b: UnitProfile, m: RadioModel): number {
  const pl = Math.max(
    FSPL_1M_DB + 10 * m.pathLossExponent * Math.log10(Math.max(d, 1)) -
      heightGainDb(a.heightM, m.heightGainCapDb) -
      heightGainDb(b.heightM, m.heightGainCapDb) +
      m.clutterDb,
    FSPL_1M_DB + 20 * Math.log10(Math.max(d, 1)),
  );
  return maxPathLossDb(a, b, m) - pl;
}

function nodes(units: PlacedUnit[], profiles: Record<UnitKind, UnitProfile>, proj: Projection): Node[] {
  return units.map((u) => {
    const [x, y] = proj.toXY(u.lng, u.lat);
    return { unit: u, profile: unitProfile(u, profiles), x, y };
  });
}

/** Links between placed units, islands, SOS relay counts and heartbeat load. */
export function analyseBackbone(
  units: PlacedUnit[],
  profiles: Record<UnitKind, UnitProfile>,
  m: RadioModel,
  proj: Projection,
): Backbone {
  const ns = nodes(units, profiles, proj);
  const n = ns.length;
  const edges: BackboneEdge[] = [];
  const adj: number[][] = ns.map(() => []);
  for (let i = 0; i < n; i++) {
    for (let j = i + 1; j < n; j++) {
      // Responder to responder is not a relay path; skip it.
      if (ns[i].unit.kind === "responder" && ns[j].unit.kind === "responder") continue;
      const d = Math.hypot(ns[i].x - ns[j].x, ns[i].y - ns[j].y);
      const marginDb = linkMarginDb(d, ns[i].profile, ns[j].profile, m);
      if (marginDb >= 0) {
        edges.push({ a: i, b: j, distanceM: d, marginDb });
        adj[i].push(j);
        adj[j].push(i);
      }
    }
  }

  const component = new Int32Array(n).fill(-1);
  let components = 0;
  for (let s = 0; s < n; s++) {
    if (component[s] >= 0) continue;
    const stack = [s];
    component[s] = components;
    while (stack.length) {
      const v = stack.pop()!;
      for (const w of adj[v]) if (component[w] < 0) {
        component[w] = components;
        stack.push(w);
      }
    }
    components++;
  }

  // Relays to a responder: BFS outward from every responder through powered units.
  const relaysToResponder = new Int32Array(n).fill(-1);
  const queue: number[] = [];
  ns.forEach((nd, i) => {
    if (nd.unit.kind === "responder") {
      relaysToResponder[i] = 0;
      queue.push(i);
    }
  });
  for (let q = 0; q < queue.length; q++) {
    const v = queue[q];
    for (const w of adj[v]) {
      if (relaysToResponder[w] >= 0 || ns[w].unit.kind !== "powered") continue;
      relaysToResponder[w] = relaysToResponder[v] + 1;
      queue.push(w);
    }
  }

  // Links to draw: Kruskal's minimum spanning tree by distance. It shows how
  // the units hang together (and any islands) without every pair in range.
  const uf = Int32Array.from({ length: n }, (_, i) => i);
  const find = (x: number): number => {
    while (uf[x] !== x) {
      uf[x] = uf[uf[x]];
      x = uf[x];
    }
    return x;
  };
  const treeEdges: number[] = [];
  const byLength = edges.map((_, k) => k).sort((x, y) => edges[x].distanceM - edges[y].distanceM);
  for (const k of byLength) {
    const ra = find(edges[k].a);
    const rb = find(edges[k].b);
    if (ra === rb) continue;
    uf[ra] = rb;
    treeEdges.push(k);
  }

  // Heartbeat flooding: every powered unit within hopLimit hops of a source relays it.
  const heartbeatSources = new Int32Array(n);
  const dist = new Int32Array(n);
  for (let s = 0; s < n; s++) {
    if (ns[s].unit.kind !== "powered") continue;
    dist.fill(-1);
    dist[s] = 0;
    const bq = [s];
    for (let q = 0; q < bq.length; q++) {
      const v = bq[q];
      if (dist[v] >= m.hopLimit) continue;
      for (const w of adj[v]) {
        if (dist[w] >= 0 || ns[w].unit.kind !== "powered") continue;
        dist[w] = dist[v] + 1;
        heartbeatSources[w]++;
        bq.push(w);
      }
    }
  }

  return { edges, component, components, relaysToResponder, heartbeatSources, treeEdges };
}

// ----------------------------------------------------------------- coverage

export interface Coverage {
  grid: Grid;
  /** Per inside cell: powered units a household unit there can reach. */
  count: Uint16Array;
  /** Per inside cell: best margin to a powered unit, dB (−Infinity when none). */
  bestMarginDb: Float32Array;
  /** Per inside cell: relays an SOS needs to reach a responder (−1 = none within the hop limit). */
  relays: Int8Array;
  stats: CoverageStats;
}

export interface CoverageStats {
  cells: number;
  areaKm2: number;
  pctCovered1: number;
  pctCovered2: number;
  pctSosReachable: number;
  hasResponders: boolean;
}

export function analyseCoverage(
  area: ProjectedArea,
  grid: Grid,
  units: PlacedUnit[],
  profiles: Record<UnitKind, UnitProfile>,
  m: RadioModel,
  proj: Projection,
  backbone: Backbone,
): Coverage {
  const civ = profiles.civilian;
  const ns = nodes(units, profiles, proj);
  const hgCiv = heightGainDb(civ.heightM, m.heightGainCapDb);
  const per = ns.map((nd) => ({
    x: nd.x,
    y: nd.y,
    r2: rangeM(civ, nd.profile, m) ** 2,
    maxPl: maxPathLossDb(civ, nd.profile, m),
    hg: heightGainDb(nd.profile.heightM, m.heightGainCapDb),
    responder: nd.unit.kind === "responder",
  }));
  const nCells = grid.inside.length;
  const count = new Uint16Array(nCells);
  const bestMarginDb = new Float32Array(nCells).fill(-Infinity);
  const relays = new Int8Array(nCells).fill(-1);
  let c1 = 0;
  let c2 = 0;
  let sos = 0;
  for (let k = 0; k < nCells; k++) {
    const [x, y] = cellXY(grid, grid.inside[k]);
    let cnt = 0;
    let best = -Infinity;
    let minRelays = Infinity;
    for (let i = 0; i < per.length; i++) {
      const p = per[i];
      const dx = p.x - x;
      const dy = p.y - y;
      const d2 = dx * dx + dy * dy;
      if (d2 > p.r2) continue;
      if (p.responder) {
        minRelays = 0;
        continue;
      }
      cnt++;
      const d = Math.max(Math.sqrt(d2), 1);
      const pl = Math.max(
        FSPL_1M_DB + 10 * m.pathLossExponent * Math.log10(d) - hgCiv - p.hg + m.clutterDb,
        FSPL_1M_DB + 20 * Math.log10(d),
      );
      const margin = p.maxPl - pl;
      if (margin > best) best = margin;
      const r = backbone.relaysToResponder[i];
      if (r > 0 && r < minRelays) minRelays = r;
    }
    count[k] = Math.min(cnt, 65535);
    bestMarginDb[k] = best;
    if (minRelays <= m.hopLimit) {
      relays[k] = minRelays;
      sos++;
    }
    if (cnt >= 1) c1++;
    if (cnt >= 2) c2++;
  }
  const pct = (v: number) => (nCells ? (100 * v) / nCells : 0);
  return {
    grid,
    count,
    bestMarginDb,
    relays,
    stats: {
      cells: nCells,
      areaKm2: area.areaM2 / 1e6,
      pctCovered1: pct(c1),
      pctCovered2: pct(c2),
      pctSosReachable: pct(sos),
      hasResponders: units.some((u) => u.kind === "responder"),
    },
  };
}

// ---------------------------------------------------------------- placement

/**
 * Powered-unit positions that give every cell `redundancy` powered units in
 * range, starting from a triangular lattice and filling the gaps greedily.
 *
 * Lattice spacing: R·√3 puts every point within R of one lattice point; R
 * puts every point within R of two (worst case is next to a lattice point,
 * whose neighbours are R away). `spacingFactor` < 1 tightens it for safety.
 */
export function autoPlace(
  area: ProjectedArea,
  grid: Grid,
  existing: PlacedUnit[],
  profiles: Record<UnitKind, UnitProfile>,
  m: RadioModel,
  proj: Projection,
  redundancy: 1 | 2,
  spacingFactor = 0.9,
  maxUnits = 3000,
): LngLat[] {
  const civ = profiles.civilian;
  const R = rangeM(civ, profiles.powered, m);
  if (R <= 0) return [];
  const R2 = R * R;
  const spacing = (redundancy === 2 ? R : R * Math.sqrt(3)) * spacingFactor;
  const placed: [number, number][] = [];

  const cx = (area.minX + area.maxX) / 2;
  const cy = (area.minY + area.maxY) / 2;
  const rowH = (spacing * Math.sqrt(3)) / 2;
  const nx = Math.ceil((area.maxX - area.minX) / spacing / 2) + 1;
  const ny = Math.ceil((area.maxY - area.minY) / rowH / 2) + 1;
  for (let j = -ny; j <= ny; j++) {
    const y = cy + j * rowH;
    const off = (Math.abs(j) % 2) * (spacing / 2);
    for (let i = -nx; i <= nx; i++) {
      const x = cx + i * spacing + off;
      if (insideArea(area, x, y)) placed.push([x, y]);
      if (placed.length >= maxUnits) break;
    }
  }

  // Coverage count per cell from existing powered units and the lattice.
  const nCells = grid.inside.length;
  const xs = new Float64Array(nCells);
  const ys = new Float64Array(nCells);
  for (let k = 0; k < nCells; k++) [xs[k], ys[k]] = cellXY(grid, grid.inside[k]);
  const count = new Uint16Array(nCells);
  const add = (px: number, py: number, r2: number) => {
    for (let k = 0; k < nCells; k++) {
      const dx = xs[k] - px;
      const dy = ys[k] - py;
      if (dx * dx + dy * dy <= r2) count[k]++;
    }
  };
  for (const u of existing) {
    if (u.kind !== "powered") continue;
    const [x, y] = proj.toXY(u.lng, u.lat);
    const r = rangeM(civ, unitProfile(u, profiles), m);
    add(x, y, r * r);
  }
  for (const [x, y] of placed) add(x, y, R2);

  // Greedy fill: centre a new unit on the uncovered cells near each gap. One
  // pass per level of redundancy, so a gap that needs two units gets them
  // from two different centres rather than two at the same spot.
  for (let pass = 0; pass < redundancy + 1; pass++) {
    for (let k = 0; k < nCells && placed.length < maxUnits; k++) {
      if (count[k] >= redundancy) continue;
      let sx = 0;
      let sy = 0;
      let n = 0;
      const lim = (R / 2) ** 2;
      for (let q = k; q < nCells; q++) {
        const dy = ys[q] - ys[k];
        if (dy * dy > lim) break; // cells are in row order, so nothing further is closer
        if (count[q] >= redundancy) continue;
        const dx = xs[q] - xs[k];
        if (dx * dx + dy * dy <= lim) {
          sx += xs[q];
          sy += ys[q];
          n++;
        }
      }
      let px = sx / n;
      let py = sy / n;
      if (!insideArea(area, px, py)) {
        px = xs[k];
        py = ys[k];
      }
      placed.push([px, py]);
      add(px, py, R2);
    }
  }

  return placed.map(([x, y]) => proj.toLngLat(x, y));
}

// ------------------------------------------------------------------- airtime

export interface AirtimeReport {
  sosS: number;
  textS: number;
  heartbeatS: number;
  budgetSPerHour: number;
  /** Worst powered unit's heartbeat transmissions per hour, upper bound (no race cancellation). */
  worstHeartbeatSPerHour: number;
  worstHeartbeatSources: number;
  /**
   * Channel time heartbeats take around the busiest powered unit when the
   * forwarding race works (§13.3): each heartbeat is sent once by its source
   * and about once per hop after that.
   */
  heartbeatChannelSPerHour: number;
}

/** Useful channel time per hour shared by everyone in range (§1.3: ~30–40% load). */
export const USABLE_CHANNEL_S_PER_HOUR = 1200;

export function airtimeReport(units: PlacedUnit[], backbone: Backbone, m: RadioModel): AirtimeReport {
  const hb = frameAirtimeS(FRAME_BYTES.heartbeat, m);
  let worst = 0;
  units.forEach((u, i) => {
    if (u.kind === "powered") worst = Math.max(worst, backbone.heartbeatSources[i]);
  });
  const anyPowered = units.some((u) => u.kind === "powered");
  return {
    sosS: frameAirtimeS(FRAME_BYTES.sos, m),
    textS: frameAirtimeS(FRAME_BYTES.text, m),
    heartbeatS: hb,
    budgetSPerHour: 3600 * (m.dutyCyclePct / 100),
    worstHeartbeatSPerHour: anyPowered ? m.heartbeatsPerHour * (1 + worst) * hb : 0,
    worstHeartbeatSources: worst,
    heartbeatChannelSPerHour: anyPowered ? m.heartbeatsPerHour * (1 + worst) * hb * (1 + m.hopLimit) : 0,
  };
}

/**
 * Mean number of household units a household unit hears directly, for the
 * "every powered unit has failed" case. A random 2-D mesh holds together as
 * one network once this passes about 4.5 (continuum percolation threshold).
 */
export function meanNeighbours(unitsPerKm2: number, rangeMetres: number): number {
  return unitsPerKm2 * Math.PI * (rangeMetres / 1000) ** 2;
}

export const PERCOLATION_MEAN_DEGREE = 4.5;
