import { test } from "node:test";
import assert from "node:assert/strict";
import { DEFAULT_COSTS, estimate, publicCosts, sumLines, validateCosts } from "../lib/costs.ts";

test("the default sheet validates and round-trips", () => {
  const v = validateCosts(JSON.parse(JSON.stringify(DEFAULT_COSTS)));
  assert.ok(v.ok);
  if (v.ok) assert.deepEqual(v.sheet, DEFAULT_COSTS);
});

test("bad sheets are refused", () => {
  const bad = (patch: object) => validateCosts({ ...structuredClone(DEFAULT_COSTS), ...patch });
  assert.equal(bad({ markupPct: -1 }).ok, false);
  assert.equal(bad({ contingencyPct: "x" }).ok, false);
  assert.equal(bad({ project: [{ name: "A", amount: 1, basis: "per_planet" }] }).ok, false);
  assert.equal(bad({ units: { ...DEFAULT_COSTS.units, civilian: [{ name: "", amount: 1 }] } }).ok, false);
  assert.equal(bad({ units: { ...DEFAULT_COSTS.units, powered: [{ name: "x", amount: -5 }] } }).ok, false);
  assert.equal(bad({ units: { ...DEFAULT_COSTS.units, powered: Array.from({ length: 41 }, () => ({ name: "x", amount: 1 })) } }).ok, false);
  assert.equal(validateCosts(null).ok, false);
});

test("names are cleaned of control characters", () => {
  const s = structuredClone(DEFAULT_COSTS);
  s.units.civilian[0].name = "Board\u0000\n<b>";
  const v = validateCosts(s);
  assert.ok(v.ok);
  if (v.ok) assert.equal(v.sheet.units.civilian[0].name, "Board  <b>");
});

test("public prices include the markup and hide the breakdown", () => {
  const s = { ...structuredClone(DEFAULT_COSTS), markupPct: 20 };
  const p = publicCosts(s, null, false);
  assert.equal(p.unitPrice.civilian, Math.round(sumLines(s.units.civilian) * 1.2 * 100) / 100);
  assert.ok(!("units" in p) && !("markupPct" in p));
});

test("estimate multiplies each line by its basis", () => {
  const p = publicCosts(DEFAULT_COSTS, null, true);
  const e = estimate(p, { civilian: 100, powered: 10, responder: 4, areaKm2: 2.5 });
  const line = (name: string) => e.lines.find((l) => l.name.startsWith(name))!;
  assert.equal(line("Household units").total, 100 * p.unitPrice.civilian);
  assert.equal(line("Site survey").quantity, 2.5);
  assert.equal(line("Field calibration").quantity, 1);
  assert.equal(line("Responder training").quantity, 4);
  assert.equal(e.contingency, Math.round(e.subtotal * 0.1 * 100) / 100);
  assert.equal(e.total, Math.round((e.subtotal + e.contingency) * 100) / 100);
});
