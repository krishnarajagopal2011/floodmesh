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
