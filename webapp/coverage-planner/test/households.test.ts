import { test } from "node:test";
import assert from "node:assert/strict";
import { analyseBackbone, insideArea, makeProjection, projectArea, type PlacedUnit, type PolygonRings } from "../lib/plan.ts";
import { analyseHouseholds, placeOnBuildings, placeRandom, SpatialIndex } from "../lib/households.ts";
import { DEFAULT_MODEL, DEFAULT_PROFILES, rangeM } from "../lib/radio.ts";

const proj = makeProjection(13, 80.2);
const at = (x: number, y = 0): [number, number] => proj.toLngLat(x, y);
const [w, s] = at(-1500, -1000);
const [e, n] = at(1500, 1000);
const rect: PolygonRings = [[[w, s], [e, s], [e, n], [w, n], [w, s]]];
const area = projectArea([rect], proj);

test("random placement: n points, all inside, same seed gives the same points", () => {
  const a = placeRandom(area, proj, 500, 42);
  const b = placeRandom(area, proj, 500, 42);
  assert.equal(a.length, 500);
  assert.deepEqual(a, b);
  for (const [lng, lat] of a) assert.ok(insideArea(area, ...proj.toXY(lng, lat)));
  assert.notDeepEqual(placeRandom(area, proj, 500, 43), a);
});

test("building placement: one per building when fewer units, shares when more, nothing outside", () => {
  const inside = Array.from({ length: 10 }, (_, i) => at(-1000 + i * 200, 0));
  const outside = [at(5000, 5000)];
  const few = placeOnBuildings([...inside, ...outside], area, proj, 4, 1);
  assert.equal(few.length, 4);
  assert.equal(new Set(few.map((p) => p.join())).size, 4, "four different buildings");
  const many = placeOnBuildings([...inside, ...outside], area, proj, 25, 1, 8);
  assert.equal(many.length, 25);
  for (const [lng, lat] of many) {
    const [x, y] = proj.toXY(lng, lat);
    const nearest = Math.min(...inside.map(([bl, bt]) => Math.hypot(...((([bx, by]) => [bx - x, by - y])(proj.toXY(bl, bt)) as [number, number]))));
    assert.ok(nearest <= 8.01, `within the spread of a building (${nearest.toFixed(1)} m)`);
  }
});

test("spatial index finds exactly the points within r", () => {
  const xs = Float64Array.from({ length: 400 }, (_, i) => (i % 20) * 37);
  const ys = Float64Array.from({ length: 400 }, (_, i) => Math.floor(i / 20) * 29);
  const idx = new SpatialIndex(xs, ys, 50);
  const got: number[] = [];
  idx.within(300, 260, 90, (i) => got.push(i));
  const want = [...xs.keys()].filter((i) => Math.hypot(xs[i] - 300, ys[i] - 260) <= 90);
  assert.deepEqual(got.sort((a, b) => a - b), want);
});

test("SOS through neighbours: a household out of powered range gets out via another household", () => {
  const m = DEFAULT_MODEL;
  const P = DEFAULT_PROFILES;
  const rHP = rangeM(P.civilian, P.powered, m);
  const rHH = rangeM(P.civilian, P.civilian, m);
  const rRP = rangeM(P.responder, P.powered, m);
  const rHR = rangeM(P.civilian, P.responder, m);
  const px = rRP * 0.8; // powered unit hears the responder
  const ax = px + rHP * 0.93; // A hears the powered unit
  const bx = ax + rHH * 0.9; // B hears A only
  assert.ok(bx - px > rHP, "B is out of the powered unit's reach");
  const units: PlacedUnit[] = [
    { id: "r", kind: "responder", name: "R1", ...ll(at(0)) },
    { id: "p", kind: "powered", name: "P1", ...ll(at(px)) },
  ];
  const points = [at(ax), at(bx), at(-rHR * 0.7), at(-5000)];
  const bb = analyseBackbone(units, P, m, proj);
  const h = analyseHouseholds(points, units, P, m, proj, bb);
  assert.deepEqual(Array.from(h.heard), [1, 0, 0, 0]);
  assert.deepEqual(Array.from(h.relays), [1, 2, 0, -1], "A: via P; B: via A and P; C: direct; D: nothing");
  assert.equal(h.stats.pctSosPoweredOnly, 50, "A and C without household relays");
  assert.equal(h.stats.pctSosWithNeighbours, 75);
  assert.equal(h.stats.pctSosNoPowered, 25, "only C, next to the responder");
  // With a hop limit of 1, B (two relays) is out.
  const h1 = analyseHouseholds(points, units, P, { ...m, hopLimit: 1 }, proj, analyseBackbone(units, P, { ...m, hopLimit: 1 }, proj));
  assert.equal(h1.relays[1], -1);
});

function ll([lng, lat]: [number, number]) {
  return { lat, lng };
}

test("building placement respects the per-building cap", () => {
  const blds = Array.from({ length: 5 }, (_, i) => at(-500 + i * 200, 0));
  const pts = placeOnBuildings(blds, area, proj, 100, 3, 8, 2);
  assert.equal(pts.length, 10, "5 buildings × 2 at most; the caller spreads the other 90");
});

test("random placement over two far-apart polygons is fast and proportional", () => {
  const sq = (cx: number, cy: number, h: number): PolygonRings => {
    const [a0, b0] = at(cx - h, cy - h);
    const [a1, b1] = at(cx + h, cy + h);
    return [[[a0, b0], [a1, b0], [a1, b1], [a0, b1], [a0, b0]]];
  };
  const two = projectArea([sq(-40_000, 0, 1000), sq(40_000, 0, 2000)], proj);
  const t = Date.now();
  const pts = placeRandom(two, proj, 5000, 9);
  assert.ok(Date.now() - t < 2000, "well under 2 s");
  assert.equal(pts.length, 5000);
  const right = pts.filter(([lng]) => proj.toXY(lng, 0)[0] > 0).length;
  // The right square is 4× the area of the left one: about 80% of the points.
  assert.ok(right > 3800 && right < 4200, `${right} of 5000 on the larger square`);
});

test("a sample is corrected to the plan's density for neighbours and groups", () => {
  const full = placeRandom(area, proj, 6000, 5);
  const sample = full.slice(0, 2000);
  const units: PlacedUnit[] = [{ id: "r", kind: "responder", name: "R1", ...ll(at(0)) }];
  const bb = analyseBackbone(units, DEFAULT_PROFILES, DEFAULT_MODEL, proj);
  const F = analyseHouseholds(full, units, DEFAULT_PROFILES, DEFAULT_MODEL, proj, bb).stats;
  const S = analyseHouseholds(sample, units, DEFAULT_PROFILES, DEFAULT_MODEL, proj, bb, 6000).stats;
  assert.equal(S.sampled, true);
  assert.equal(F.sampled, false);
  assert.ok(Math.abs(S.meanNeighbours - F.meanNeighbours) / F.meanNeighbours < 0.1, `${S.meanNeighbours} vs ${F.meanNeighbours}`);
  assert.ok(Math.abs(S.pctLargestGroup - F.pctLargestGroup) < 10, `${S.pctLargestGroup} vs ${F.pctLargestGroup}`);
  // Hop-limited reach from a sample is a lower bound.
  assert.ok(S.pctSosNoPowered <= F.pctSosNoPowered + 1);
});

test("each placed household unit gets its best margin to a powered unit", () => {
  const units: PlacedUnit[] = [{ id: "p", kind: "powered", name: "P1", ...ll(at(0)) }];
  const bb = analyseBackbone(units, DEFAULT_PROFILES, DEFAULT_MODEL, proj);
  const h = analyseHouseholds([at(100), at(50_000)], units, DEFAULT_PROFILES, DEFAULT_MODEL, proj, bb);
  assert.ok(Number.isFinite(h.bestMarginDb[0]) && h.bestMarginDb[0] > 0);
  assert.equal(h.bestMarginDb[1], -Infinity);
});

test("Overpass errors reported with HTTP 200 are not read as an answer", async () => {
  const { buildingCentres, countBuildings } = await import("../lib/osm.ts");
  const real = globalThis.fetch;
  globalThis.fetch = (async () =>
    new Response(JSON.stringify({ elements: [{ type: "way", id: 1, center: { lat: 13, lon: 80 } }], remark: "runtime error: Query timed out in \"query\" at line 1 after 181 seconds." }), {
      status: 200,
      headers: { "content-type": "application/json" },
    })) as typeof fetch;
  try {
    await assert.rejects(buildingCentres([rect]), /stopped early/);
    await assert.rejects(countBuildings([rect]), /stopped early/);
    globalThis.fetch = (async () => new Response("<html>busy</html>", { status: 200 })) as typeof fetch;
    await assert.rejects(buildingCentres([rect]), /unexpected reply/);
  } finally {
    globalThis.fetch = real;
  }
});
