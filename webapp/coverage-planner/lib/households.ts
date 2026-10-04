/**
 * Household units as individual points: where they go, and what each one
 * can reach.
 *
 * The grid coverage (plan.ts) asks "could a household unit here hear a
 * powered unit?". Placed household units answer two more questions:
 * - SOS through neighbours: every unit forwards SOS (architecture §13.3), so
 *   a household unit out of reach of every powered unit may still get its
 *   SOS out through other household units, within the hop limit.
 * - The fallback: if every powered unit fails, how many household units can
 *   still reach a responder through each other alone.
 *
 * A constituency can have tens of thousands of household units, so at most
 * MAX_PLACED are placed: a random sample spread like the real ones. A sample
 * is sparser than reality, which makes the through-neighbours figures
 * conservative (fewer neighbours, fewer paths).
 *
 * Pure functions, shared by the browser and the tests.
 */
import {
  insideArea,
  unitProfile,
  type Backbone,
  type LngLat,
  type PlacedUnit,
  type ProjectedArea,
  type Projection,
} from "./plan.ts";
import { rangeM, type RadioModel, type UnitKind, type UnitProfile } from "./radio.ts";

export const MAX_PLACED = 20_000;

export interface HouseholdPoints {
  /** "osm": on OpenStreetMap building positions; "random": spread evenly over the area. */
  source: "osm" | "random";
  /** [lng, lat], rounded to about 10 cm. */
  points: LngLat[];
  /** Household units the plan called for when these were placed (more than points.length for a sample). */
  total: number;
}

// ------------------------------------------------------------- placement

/** Small, fast, seedable PRNG (mulberry32): the same area gives the same dots. */
export function rng(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export function seedOf(text: string): number {
  let h = 2166136261;
  for (let i = 0; i < text.length; i++) h = Math.imul(h ^ text.charCodeAt(i), 16777619);
  return h >>> 0;
}

const round7 = (v: number) => Math.round(v * 1e7) / 1e7;

/** `n` points spread uniformly over the area (rejection sampling in its bounding box). */
export function placeRandom(area: ProjectedArea, proj: Projection, n: number, seed: number): LngLat[] {
  const rand = rng(seed);
  const out: LngLat[] = [];
  const w = area.maxX - area.minX;
  const h = area.maxY - area.minY;
  for (let tries = 0; out.length < n && tries < n * 200 + 1000; tries++) {
    const x = area.minX + rand() * w;
    const y = area.minY + rand() * h;
    if (!insideArea(area, x, y)) continue;
    const [lng, lat] = proj.toLngLat(x, y);
    out.push([round7(lng), round7(lat)]);
  }
  return out;
}

/**
 * `n` points on building positions inside the area. Fewer units than
 * buildings: a random choice of buildings. More: every building gets its
 * share (apartment blocks hold several households), spread a few metres
 * around its centre so the dots stay visible.
 */
export function placeOnBuildings(
  buildings: LngLat[],
  area: ProjectedArea,
  proj: Projection,
  n: number,
  seed: number,
  spreadM = 8,
): LngLat[] {
  const rand = rng(seed);
  const inside = buildings
    .map(([lng, lat]) => proj.toXY(lng, lat))
    .filter(([x, y]) => insideArea(area, x, y));
  const B = inside.length;
  if (!B || n <= 0) return [];
  const perBuilding = new Uint32Array(B);
  if (n <= B) {
    // Partial Fisher-Yates: n distinct buildings.
    const order = Uint32Array.from({ length: B }, (_, i) => i);
    for (let i = 0; i < n; i++) {
      const j = i + Math.floor(rand() * (B - i));
      [order[i], order[j]] = [order[j], order[i]];
      perBuilding[order[i]] = 1;
    }
  } else {
    const each = Math.floor(n / B);
    perBuilding.fill(each);
    for (let left = n - each * B; left > 0; left--) perBuilding[Math.floor(rand() * B)]++;
  }
  const out: LngLat[] = [];
  for (let b = 0; b < B; b++) {
    const [bx, by] = inside[b];
    for (let k = 0; k < perBuilding[b]; k++) {
      // The first unit sits on the building; the rest are spread around it.
      const r = k === 0 ? 0 : spreadM * Math.sqrt(rand());
      const t = rand() * 2 * Math.PI;
      const [lng, lat] = proj.toLngLat(bx + r * Math.cos(t), by + r * Math.sin(t));
      out.push([round7(lng), round7(lat)]);
    }
  }
  return out;
}

// --------------------------------------------------------------- analysis

/** Points bucketed into square cells, for "everything within r of (x, y)". */
export class SpatialIndex {
  private cells = new Map<number, number[]>();
  private xs: ArrayLike<number>;
  private ys: ArrayLike<number>;
  private size: number;
  constructor(xs: ArrayLike<number>, ys: ArrayLike<number>, size: number) {
    this.xs = xs;
    this.ys = ys;
    this.size = Math.max(size, 1);
    for (let i = 0; i < xs.length; i++) {
      const k = this.key(Math.floor(xs[i] / this.size), Math.floor(ys[i] / this.size));
      const c = this.cells.get(k);
      if (c) c.push(i);
      else this.cells.set(k, [i]);
    }
  }

  private key(bx: number, by: number): number {
    // Unique while bucket indices stay within ±2^20 (over 1,000 km at 1 m
    // buckets); beyond that a collision only costs extra distance checks.
    return (bx + 1_048_576) * 2_097_152 + (by + 1_048_576);
  }

  /** Calls fn(i, d²) for every point within r of (x, y). */
  within(x: number, y: number, r: number, fn: (i: number, d2: number) => void): void {
    const s = this.size;
    const r2 = r * r;
    const bx0 = Math.floor((x - r) / s);
    const bx1 = Math.floor((x + r) / s);
    const by0 = Math.floor((y - r) / s);
    const by1 = Math.floor((y + r) / s);
    for (let bx = bx0; bx <= bx1; bx++) {
      for (let by = by0; by <= by1; by++) {
        const c = this.cells.get(this.key(bx, by));
        if (!c) continue;
        for (const i of c) {
          const dx = this.xs[i] - x;
          const dy = this.ys[i] - y;
          const d2 = dx * dx + dy * dy;
          if (d2 <= r2) fn(i, d2);
        }
      }
    }
  }
}

export interface HouseholdStats {
  placed: number;
  pctHeard1: number;
  pctHeard2: number;
  /** SOS reaches a responder with powered units as the only relays. */
  pctSosPoweredOnly: number;
  /** SOS reaches a responder when household units relay too (§13.3). */
  pctSosWithNeighbours: number;
  /** With every powered unit down: SOS still reaches a responder through household units. */
  pctSosNoPowered: number;
  /** With every powered unit down: share of household units in the largest connected group. */
  pctLargestGroup: number;
  /** Household units each one hears directly, on average. */
  meanNeighbours: number;
  hasResponders: boolean;
}

export interface HouseholdAnalysis {
  /** Per placed household unit: powered units it hears. */
  heard: Uint16Array;
  /** Per placed household unit: relays its SOS needs with neighbours relaying (−1 = not within the hop limit). */
  relays: Int16Array;
  stats: HouseholdStats;
  ms: number;
}

/**
 * What every placed household unit can reach. Graph nodes are the household
 * units and the placed units; links follow the same radio model as the rest
 * of the planner (household-household at the indoor-to-indoor range,
 * household-unit at that unit's range, unit-unit from the backbone).
 */
export function analyseHouseholds(
  points: LngLat[],
  units: PlacedUnit[],
  profiles: Record<UnitKind, UnitProfile>,
  m: RadioModel,
  proj: Projection,
  backbone: Backbone,
): HouseholdAnalysis {
  const t0 = typeof performance !== "undefined" ? performance.now() : Date.now();
  const H = points.length;
  const U = units.length;
  const hx = new Float64Array(H);
  const hy = new Float64Array(H);
  for (let i = 0; i < H; i++) [hx[i], hy[i]] = proj.toXY(points[i][0], points[i][1]);
  const ux = new Float64Array(U);
  const uy = new Float64Array(U);
  const ur = new Float64Array(U); // each unit's reach to a household unit
  const civ = profiles.civilian;
  let maxUnitR = 0;
  units.forEach((u, i) => {
    [ux[i], uy[i]] = proj.toXY(u.lng, u.lat);
    ur[i] = rangeM(civ, unitProfile(u, profiles), m);
    if (ur[i] > maxUnitR) maxUnitR = ur[i];
  });
  const rHH = rangeM(civ, civ, m);
  const hIndex = new SpatialIndex(hx, hy, Math.max(rHH, 25));
  const uIndex = new SpatialIndex(ux, uy, Math.max(maxUnitR, 25));
  const unitAdj: number[][] = units.map(() => []);
  for (const e of backbone.edges) {
    unitAdj[e.a].push(e.b);
    unitAdj[e.b].push(e.a);
  }
  const isPowered = units.map((u) => u.kind === "powered");

  // Powered units each household unit hears.
  const heard = new Uint16Array(H);
  let heard1 = 0;
  let heard2 = 0;
  for (let h = 0; h < H; h++) {
    let n = 0;
    uIndex.within(hx[h], hy[h], maxUnitR, (u, d2) => {
      if (isPowered[u] && d2 <= ur[u] * ur[u]) n++;
    });
    heard[h] = Math.min(n, 65535);
    if (n >= 1) heard1++;
    if (n >= 2) heard2++;
  }

  // Nodes 0..H-1 are household units, H..H+U-1 placed units. BFS outward
  // from the responders; a node's relays = its distance − 1.
  const N = H + U;
  const dist = new Int32Array(N);
  const bfs = (hhLinks: boolean, poweredUp: boolean): Int32Array => {
    dist.fill(-1);
    const queue = new Int32Array(N);
    let qh = 0;
    let qt = 0;
    for (let u = 0; u < U; u++) {
      if (units[u].kind === "responder") {
        dist[H + u] = 0;
        queue[qt++] = H + u;
      }
    }
    // A node at distance d relays for its neighbours; the SOS may pass at
    // most hopLimit relays, so nothing beyond distance hopLimit + 1 counts.
    const maxDist = m.hopLimit + 1;
    while (qh < qt) {
      const v = queue[qh++];
      const dv = dist[v];
      if (dv >= maxDist) continue;
      const visit = (w: number) => {
        if (dist[w] >= 0) return;
        dist[w] = dv + 1;
        queue[qt++] = w;
      };
      if (v < H) {
        // Without household relaying, a household unit is only ever an end point.
        if (!hhLinks) continue;
        hIndex.within(hx[v], hy[v], rHH, (w) => visit(w));
        uIndex.within(hx[v], hy[v], maxUnitR, (u, d2) => {
          if (d2 <= ur[u] * ur[u] && (poweredUp || !isPowered[u])) visit(H + u);
        });
      } else {
        const u = v - H;
        // A relay must be a working unit; the responders are where SOS ends.
        if (!poweredUp && isPowered[u]) continue;
        // Responders hand nothing on: an SOS that reached one has arrived.
        if (units[u].kind === "responder" && dv > 0) continue;
        for (const w of unitAdj[u]) if (poweredUp || !isPowered[w]) visit(H + w);
        hIndex.within(ux[u], uy[u], ur[u], (w) => visit(w));
      }
    }
    return dist;
  };

  const share = (d: Int32Array) => {
    let n = 0;
    for (let h = 0; h < H; h++) if (d[h] >= 1) n++;
    return n;
  };
  const poweredOnly = share(bfs(false, true));
  const withNeighbours = bfs(true, true);
  const relays = new Int16Array(H);
  let withN = 0;
  for (let h = 0; h < H; h++) {
    relays[h] = withNeighbours[h] >= 1 ? withNeighbours[h] - 1 : -1;
    if (relays[h] >= 0) withN++;
  }
  const noPowered = share(bfs(true, false));

  // Largest group of household units linked to each other (every powered unit down).
  const group = new Int32Array(H).fill(-1);
  let largest = 0;
  let links = 0;
  const stack: number[] = [];
  for (let s = 0; s < H; s++) {
    if (group[s] >= 0) continue;
    group[s] = s;
    stack.push(s);
    let size = 0;
    while (stack.length) {
      const v = stack.pop()!;
      size++;
      hIndex.within(hx[v], hy[v], rHH, (w) => {
        if (w === v) return;
        links++;
        if (group[w] < 0) {
          group[w] = s;
          stack.push(w);
        }
      });
    }
    if (size > largest) largest = size;
  }

  const pct = (v: number) => (H ? (100 * v) / H : 0);
  const t1 = typeof performance !== "undefined" ? performance.now() : Date.now();
  return {
    heard,
    relays,
    stats: {
      placed: H,
      pctHeard1: pct(heard1),
      pctHeard2: pct(heard2),
      pctSosPoweredOnly: pct(poweredOnly),
      pctSosWithNeighbours: pct(withN),
      pctSosNoPowered: pct(noPowered),
      pctLargestGroup: pct(largest),
      meanNeighbours: H ? links / H : 0,
      hasResponders: units.some((u) => u.kind === "responder"),
    },
    ms: t1 - t0,
  };
}
