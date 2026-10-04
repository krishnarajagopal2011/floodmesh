import { test } from "node:test";
import assert from "node:assert/strict";
import {
  analyseBackbone,
  analyseCoverage,
  autoPlace,
  buildGrid,
  makeProjection,
  pickCellSize,
  projectArea,
  type PlacedUnit,
  type PolygonRings,
} from "../lib/plan.ts";
import { DEFAULT_MODEL, DEFAULT_PROFILES, rangeM } from "../lib/radio.ts";
import { normName } from "../lib/names.ts";

const lat0 = 13.0;
const lng0 = 80.2;
const proj = makeProjection(lat0, lng0);
// A 3 km × 2 km rectangle around the projection centre.
const [w, s] = proj.toLngLat(-1500, -1000);
const [e, n] = proj.toLngLat(1500, 1000);
const rect: PolygonRings = [[[w, s], [e, s], [e, n], [w, n], [w, s]]];

test("projected area and grid", () => {
  const a = projectArea([rect], proj);
  assert.ok(Math.abs(a.areaM2 - 6e6) / 6e6 < 0.001, `area ${a.areaM2}`);
  const cell = pickCellSize(a.areaM2);
  const g = buildGrid(a, cell);
  const covered = g.inside.length * cell * cell;
  assert.ok(Math.abs(covered - 6e6) / 6e6 < 0.05, `grid area ${covered}`);
});

test("a hole is not inside", () => {
  const [hw, hs] = proj.toLngLat(-200, -200);
  const [he, hn] = proj.toLngLat(200, 200);
  const withHole: PolygonRings = [rect[0], [[hw, hs], [he, hs], [he, hn], [hw, hn], [hw, hs]]];
  const a = projectArea([withHole], proj);
  const g = buildGrid(a, 50);
  assert.ok(Math.abs(g.inside.length * 2500 - (6e6 - 160_000)) / 6e6 < 0.02);
});

for (const red of [1, 2] as const) {
  test(`auto-place covers the whole area with redundancy ${red}`, () => {
    const a = projectArea([rect], proj);
    const g = buildGrid(a, 50);
    const pts = autoPlace(a, g, [], DEFAULT_PROFILES, DEFAULT_MODEL, proj, red, 0.9);
    assert.ok(pts.length > 0);
    const units: PlacedUnit[] = pts.map(([lng, lat], i) => ({ id: `p${i}`, kind: "powered", lat, lng, name: `P${i + 1}` }));
    const bb = analyseBackbone(units, DEFAULT_PROFILES, DEFAULT_MODEL, proj);
    const cov = analyseCoverage(a, g, units, DEFAULT_PROFILES, DEFAULT_MODEL, proj, bb);
    const pct = red === 1 ? cov.stats.pctCovered1 : cov.stats.pctCovered2;
    assert.equal(pct, 100, `coverage ${pct}% with ${pts.length} units`);
    // Lattice density sanity: not wildly more than area / (hexagon of the needed spacing).
    const R = rangeM(DEFAULT_PROFILES.civilian, DEFAULT_PROFILES.powered, DEFAULT_MODEL);
    const sp = (red === 2 ? R : R * Math.sqrt(3)) * 0.9;
    const ideal = 6e6 / ((Math.sqrt(3) / 2) * sp * sp);
    assert.ok(pts.length < ideal * 2.5 + 8, `${pts.length} units vs about ${ideal.toFixed(1)} ideal`);
  });
}

test("relays to a responder are counted along a chain", () => {
  const R = rangeM(DEFAULT_PROFILES.powered, DEFAULT_PROFILES.powered, DEFAULT_MODEL);
  const at = (x: number): [number, number] => {
    const [lng, lat] = proj.toLngLat(x, 0);
    return [lat, lng];
  };
  const step = R * 0.8;
  const units: PlacedUnit[] = [
    { id: "r", kind: "responder", name: "R1", ...pos(at(-rangeM(DEFAULT_PROFILES.responder, DEFAULT_PROFILES.powered, DEFAULT_MODEL) * 0.5)) },
    ...[0, 1, 2, 3, 4].map((i) => ({ id: `p${i}`, kind: "powered" as const, name: `P${i + 1}`, ...pos(at(i * step)) })),
  ];
  const bb = analyseBackbone(units, DEFAULT_PROFILES, DEFAULT_MODEL, proj);
  assert.deepEqual(Array.from(bb.relaysToResponder), [0, 1, 2, 3, 4, 5]);
  assert.equal(bb.components, 1);
  // Heartbeats from P1 reach P2..P4 within 3 hops; P3 hears from all four others.
  assert.equal(bb.heartbeatSources[3], 4);
  // Drawn links: a spanning tree over the six units.
  assert.equal(bb.treeEdges.length, 5);
});

test("drawn links are a spanning tree, not every pair in range", () => {
  const units: PlacedUnit[] = [0, 1, 2, 3].map((i) => {
    const [lng, lat] = proj.toLngLat(i * 300, 0);
    return { id: `p${i}`, kind: "powered" as const, name: `P${i + 1}`, lat, lng };
  });
  const bb = analyseBackbone(units, DEFAULT_PROFILES, DEFAULT_MODEL, proj);
  assert.equal(bb.edges.length, 6, "all pairs hear each other");
  assert.equal(bb.treeEdges.length, 3, "a tree over 4 units");
  assert.ok(Array.from(bb.relaysToResponder).every((r) => r === -1));
});

function pos([lat, lng]: [number, number]) {
  return { lat, lng };
}

test("constituency names match common spellings", () => {
  assert.equal(normName("Madhuravoyal"), normName("Maduravoyal"));
  assert.equal(normName("Sholinganallur"), normName("Shozhinganallur"));
  assert.equal(normName("Thiruvottiyur"), normName("Tiruvottiyur"));
  assert.ok(normName("Alandur").includes(normName("aland")));
});

test("grid size is capped by the bounding box, not only the area", async () => {
  const { gridCellSize } = await import("../lib/plan.ts");
  const sq = (lng: number, lat: number): PolygonRings => [[[lng, lat], [lng + 0.001, lat], [lng + 0.001, lat + 0.001], [lng, lat + 0.001], [lng, lat]]];
  const a = projectArea([sq(80, 13), sq(85, 18)], proj);
  const g = buildGrid(a, gridCellSize(a));
  assert.ok(g.cols * g.rows <= 450_000, `${g.cols * g.rows} cells`);
  // A normal area keeps the fine grid.
  const r = projectArea([rect], proj);
  assert.equal(gridCellSize(r), pickCellSize(r.areaM2));
});

test("auto-place with units kept adds only what is missing", () => {
  const a = projectArea([rect], proj);
  const g = buildGrid(a, 50);
  const first = autoPlace(a, g, [], DEFAULT_PROFILES, DEFAULT_MODEL, proj, 2, 0.9);
  const kept: PlacedUnit[] = first.map(([lng, lat], i) => ({ id: `p${i}`, kind: "powered", lat, lng, name: `P${i + 1}` }));
  const again = autoPlace(a, g, kept, DEFAULT_PROFILES, DEFAULT_MODEL, proj, 2, 0.9);
  assert.equal(again.length, 0, `added ${again.length} on top of a complete plan`);
});

test("auto-place respects the unit cap, filling from the centre", () => {
  const a = projectArea([rect], proj);
  const g = buildGrid(a, 50);
  const pts = autoPlace(a, g, [], DEFAULT_PROFILES, DEFAULT_MODEL, proj, 2, 0.9, 3);
  assert.equal(pts.length, 3);
  for (const [lng, lat] of pts) {
    const [x, y] = proj.toXY(lng, lat);
    assert.ok(Math.hypot(x, y) < 1000, "near the centre");
  }
});

test("plan files: bad coordinates dropped, unit ids made unique, names cleaned", async () => {
  const { fromPlanFile, sanitizePolygons, MAX_AREA_VERTICES } = await import("../lib/planState.ts");
  const p = fromPlanFile({
    app: "floodmesh-coverage-planner",
    area: { name: "x", polygons: [[[[80, 13], [80.01, 13], [80.01, 13.01], [80, 13.01], [80, 13]], [[1, 2]]], [[[999, 13], [80, 13], [80, 1], [80, 13]]]] },
    units: [
      { id: "a", kind: "powered", lat: 13, lng: 80, name: "=cmd\u0007" },
      { id: "a", kind: "powered", lat: 13, lng: 80 },
      { id: "", kind: "responder", lat: 13, lng: 80 },
      { id: "u2", kind: "responder", lat: 13, lng: 80 },
    ],
  });
  assert.equal(p.area!.polygons.length, 1, "the polygon with an off-map vertex is dropped");
  assert.equal(p.area!.polygons[0].length, 1, "the 2-point hole is dropped");
  const ids = p.units.map((u) => u.id);
  assert.equal(new Set(ids).size, ids.length, ids.join(","));
  assert.equal(p.units[0].name, "=cmd ");
  // Very detailed outlines are thinned.
  const big: [number, number][] = Array.from({ length: 20_000 }, (_, i) => [80 + Math.cos(i / 3183) * 0.1, 13 + Math.sin(i / 3183) * 0.1]);
  big.push(big[0]);
  const thin = sanitizePolygons([[big]]);
  assert.ok(thin[0][0].length <= MAX_AREA_VERTICES + 2);
});

test("CSV export neutralises spreadsheet formulas", async () => {
  const { toCsv } = await import("../lib/exporters.ts");
  const csv = toCsv(
    [
      { id: "1", kind: "powered", lat: 13, lng: 80, name: '=HYPERLINK("http://x","y")' },
      { id: "2", kind: "powered", lat: -13, lng: -80, name: "@SUM(1)" },
      { id: "3", kind: "powered", lat: 13, lng: 80, name: "P3" },
    ],
    { powered: DEFAULT_PROFILES.powered, responder: DEFAULT_PROFILES.responder },
  );
  const lines = csv.trim().split("\n");
  assert.ok(lines[1].startsWith(`"'=HYPERLINK`), lines[1]);
  assert.ok(lines[2].startsWith("'@SUM"), lines[2]);
  assert.ok(lines[2].includes(",-13.000000,-80.000000,"), "negative coordinates stay numbers");
  assert.ok(lines[3].startsWith("P3,"));
});
