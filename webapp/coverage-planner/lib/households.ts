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
 * is sparser than reality. The neighbour count and the largest-group figure
 * are corrected for that (density scaling, below); the hop-limited SOS
 * figures cannot be, so for a sample they are lower bounds.
 *
 * Pure functions, shared by the browser and the tests.
 */
import {
  FSPL_1M_DB,
  heightGainDb,
  maxPathLossDb,
  rangeM,
  type RadioModel,
  type UnitKind,
  type UnitProfile,
} from "./radio.ts";
import {
  inRing,
  insideArea,
  ringArea,
  unitProfile,
  type Backbone,
  type LngLat,
  type PlacedUnit,
  type ProjectedArea,
  type Projection,
} from "./plan.ts";

export const MAX_PLACED = 20_000;

export interface HouseholdPoints {
  /** "osm": on OpenStreetMap building positions; "random": spread evenly over the area. */
  source: "osm" | "random";
  /** [lng, lat], rounded to about 10 cm. */
  points: LngLat[];
  /** Household units the plan called for when these were placed (more than points.length for a sample). */
  total: number;
  /**
   * For "osm": how many of the points sit on mapped buildings. The rest are
   * spread evenly, because OSM rarely maps every building and piling all
   * units onto the mapped few would misplace them.
   */
  onBuildings?: number;
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

/**
 * `n` points spread uniformly over the area. Each point picks a polygon in
 * proportion to its area, then is sampled in that polygon's own bounding
 * box: far-apart polygons (two constituencies that don't touch) do not make
 * most tries miss, and each test walks only one polygon's outline.
 */
export function placeRandom(area: ProjectedArea, proj: Projection, n: number, seed: number): LngLat[] {
  const rand = rng(seed);
  const polys = area.polygons
    .map((rings) => {
      let minX = Infinity;
      let minY = Infinity;
      let maxX = -Infinity;
      let maxY = -Infinity;
      const outer = rings[0];
      for (let i = 0; i < outer.length; i += 2) {
        if (outer[i] < minX) minX = outer[i];
        if (outer[i] > maxX) maxX = outer[i];
        if (outer[i + 1] < minY) minY = outer[i + 1];
        if (outer[i + 1] > maxY) maxY = outer[i + 1];
      }
      const a = Math.abs(ringArea(outer)) - rings.slice(1).reduce((s, r) => s + Math.abs(ringArea(r)), 0);
      return { rings, minX, minY, maxX, maxY, area: Math.max(a, 0) };
    })
    .filter((p) => p.area > 0);
  const totalArea = polys.reduce((s, p) => s + p.area, 0);
  if (!polys.length || totalArea <= 0) return [];
  const cum: number[] = [];
  let acc = 0;
  for (const p of polys) cum.push((acc += p.area / totalArea));
  const inside = (p: (typeof polys)[number], x: number, y: number) => {
    if (!inRing(p.rings[0], x, y)) return false;
    for (let k = 1; k < p.rings.length; k++) if (inRing(p.rings[k], x, y)) return false;
    return true;
  };
  const out: LngLat[] = [];
  for (let tries = 0; out.length < n && tries < n * 100 + 1000; tries++) {
    const u = rand();
    let j = 0;
    while (j < cum.length - 1 && u > cum[j]) j++;
    const p = polys[j];
    const x = p.minX + rand() * (p.maxX - p.minX);
    const y = p.minY + rand() * (p.maxY - p.minY);
    if (!inside(p, x, y)) continue;
    const [lng, lat] = proj.toLngLat(x, y);
    out.push([round7(lng), round7(lat)]);
  }
  return out;
}

/**
 * Up to `n` points on building positions inside the area. Fewer units than
 * buildings: a random choice of buildings. More: every building gets its
 * share (apartment blocks hold several households), but at most
 * `maxPerBuilding`, spread a few metres around its centre so the dots stay
 * visible. Returns fewer than `n` when the cap binds: the caller spreads the
 * rest evenly.
 */
export function placeOnBuildings(
  buildings: LngLat[],
  area: ProjectedArea,
  proj: Projection,
  n: number,
  seed: number,
  spreadM = 8,
  maxPerBuilding = Infinity,
): LngLat[] {
  const rand = rng(seed);
  const inside = buildings
    .map(([lng, lat]) => proj.toXY(lng, lat))
    .filter(([x, y]) => insideArea(area, x, y));
  const B = inside.length;
  if (!B || n <= 0) return [];
  const perBuilding = new Uint32Array(B);
  n = Math.min(n, Math.floor(B * Math.max(maxPerBuilding, 1)));
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
    // The remainder goes to distinct buildings (never past the cap).
    const order = Uint32Array.from({ length: B }, (_, i) => i);
    const left = n - each * B;
    for (let i = 0; i < left; i++) {
      const j = i + Math.floor(rand() * (B - i));
      [order[i], order[j]] = [order[j], order[i]];
      perBuilding[order[i]]++;
    }
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
  /** Household units each one hears directly, on average (at the plan's full density). */
  meanNeighbours: number;
  hasResponders: boolean;
  /** True when the points are a thinned sample: the SOS-through-neighbours figures are then lower bounds. */
  sampled: boolean;
}

export interface HouseholdAnalysis {
  /** Per placed household unit: powered units it hears. */
  heard: Uint16Array;
  /** Per placed household unit: best link margin to a powered unit, dB (−Infinity when none). */
  bestMarginDb: Float32Array;
  /** Per placed household unit: relays its SOS needs with neighbours relaying (−1 = not within the hop limit). */
  relays: Int16Array;
  stats: HouseholdStats;
  ms: number;
}

const groupCache = new WeakMap<LngLat[], { key: string; largest: number; links: number }>();

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
  /** Household units the plan has; more than points.length when the points are a sample. */
  total: number = points.length,
): HouseholdAnalysis {
  const t0 = typeof performance !== "undefined" ? performance.now() : Date.now();
  const H = points.length;
  const densityScale = H ? Math.max(1, total / H) : 1;
  const U = units.length;
  const hx = new Float64Array(H);
  const hy = new Float64Array(H);
  for (let i = 0; i < H; i++) [hx[i], hy[i]] = proj.toXY(points[i][0], points[i][1]);
  const ux = new Float64Array(U);
  const uy = new Float64Array(U);
  const ur = new Float64Array(U); // each unit's reach to a household unit
  const uMaxPl = new Float64Array(U);
  const uHg = new Float64Array(U);
  const civ = profiles.civilian;
  let maxUnitR = 0;
  units.forEach((u, i) => {
    [ux[i], uy[i]] = proj.toXY(u.lng, u.lat);
    const prof = unitProfile(u, profiles);
    ur[i] = rangeM(civ, prof, m);
    uMaxPl[i] = maxPathLossDb(civ, prof, m);
    uHg[i] = heightGainDb(prof.heightM, m.heightGainCapDb);
    if (ur[i] > maxUnitR) maxUnitR = ur[i];
  });
  const hgCiv = heightGainDb(civ.heightM, m.heightGainCapDb);
  const rHH = rangeM(civ, civ, m);
  const hIndex = new SpatialIndex(hx, hy, Math.max(rHH, 25));
  const uIndex = new SpatialIndex(ux, uy, Math.max(maxUnitR, 25));
  const unitAdj: number[][] = units.map(() => []);
  for (const e of backbone.edges) {
    unitAdj[e.a].push(e.b);
    unitAdj[e.b].push(e.a);
  }
  const isPowered = units.map((u) => u.kind === "powered");

  // Powered units each household unit hears, and the best margin (same
  // path-loss formula as the coverage grid).
  const heard = new Uint16Array(H);
  const bestMarginDb = new Float32Array(H).fill(-Infinity);
  let heard1 = 0;
  let heard2 = 0;
  for (let h = 0; h < H; h++) {
    let n = 0;
    let best = -Infinity;
    uIndex.within(hx[h], hy[h], maxUnitR, (u, d2) => {
      if (!isPowered[u] || d2 > ur[u] * ur[u]) return;
      n++;
      const d = Math.max(Math.sqrt(d2), 1);
      const pl = Math.max(
        FSPL_1M_DB + 10 * m.pathLossExponent * Math.log10(d) - hgCiv - uHg[u] + m.clutterDb,
        FSPL_1M_DB + 20 * Math.log10(d),
      );
      if (uMaxPl[u] - pl > best) best = uMaxPl[u] - pl;
    });
    bestMarginDb[h] = best;
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

  // Largest group of household units linked to each other (every powered
  // unit down), and the mean neighbour count. Neither depends on the placed
  // units, so the result is cached per set of points while units move.
  // For a sample, both are taken at the plan's full density: the neighbour
  // count is scaled up, and groups are found with the range scaled by
  // √(total / placed), which for evenly spread points gives the same
  // connectivity as the full set.
  const rGroup = rHH * Math.sqrt(densityScale);
  const groupKey = `${rHH}|${rGroup}|${H}`;
  let cached = groupCache.get(points);
  if (!cached || cached.key !== groupKey) {
    const gIndex = densityScale > 1 ? new SpatialIndex(hx, hy, Math.max(rGroup, 25)) : hIndex;
    const group = new Int32Array(H).fill(-1);
    let largest = 0;
    let links = 0;
    const rHH2 = rHH * rHH;
    const stack: number[] = [];
    for (let s0 = 0; s0 < H; s0++) {
      if (group[s0] >= 0) continue;
      group[s0] = s0;
      stack.push(s0);
      let size = 0;
      while (stack.length) {
        const v = stack.pop()!;
        size++;
        gIndex.within(hx[v], hy[v], rGroup, (w, d2) => {
          if (w === v) return;
          if (d2 <= rHH2) links++;
          if (group[w] < 0) {
            group[w] = s0;
            stack.push(w);
          }
        });
      }
      if (size > largest) largest = size;
    }
    cached = { key: groupKey, largest, links };
    groupCache.set(points, cached);
  }
  const largest = cached.largest;
  const links = cached.links;

  const pct = (v: number) => (H ? (100 * v) / H : 0);
  const t1 = typeof performance !== "undefined" ? performance.now() : Date.now();
  return {
    heard,
    bestMarginDb,
    relays,
    stats: {
      placed: H,
      pctHeard1: pct(heard1),
      pctHeard2: pct(heard2),
      pctSosPoweredOnly: pct(poweredOnly),
      pctSosWithNeighbours: pct(withN),
      pctSosNoPowered: pct(noPowered),
      pctLargestGroup: pct(largest),
      meanNeighbours: H ? (links / H) * densityScale : 0,
      hasResponders: units.some((u) => u.kind === "responder"),
      sampled: densityScale > 1,
    },
    ms: t1 - t0,
  };
}
