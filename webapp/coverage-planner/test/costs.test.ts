import { test } from "node:test";
import assert from "node:assert/strict";
import { estimate, publicCosts, sumLines, validateCosts } from "../lib/costs.ts";
import { DEFAULT_COSTS } from "../lib/defaultCosts.ts";

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
  // A public fee (WPC) is not marked up, so it cannot reveal the markup.
  assert.equal(p.project.find((l) => l.id === "wpc")!.rate, 10000);
  assert.equal(p.project.find((l) => l.id === "survey")!.rate, 3600);
});

test("sheets saved before the markup flag mark every project line up", () => {
  const s = structuredClone(DEFAULT_COSTS) as unknown as { project: Record<string, unknown>[] };
  for (const l of s.project) delete l.markup;
  const v = validateCosts(s);
  assert.ok(v.ok);
  if (v.ok) assert.ok(v.sheet.project.every((l) => l.markup === true));
});

test("line ids come back unique", () => {
  const s = structuredClone(DEFAULT_COSTS);
  s.units.civilian = [
    { id: "a", name: "One", amount: 1, note: "" },
    { id: "a", name: "Two", amount: 2, note: "" },
    { id: "line3", name: "Three", amount: 3, note: "" },
    { id: "", name: "Four", amount: 4, note: "" },
    { id: "", name: "Five", amount: 5, note: "" },
  ];
  // The 4th line's fallback is line4; make the 5th line's fallback collide with an explicit id.
  s.units.civilian[2].id = "line5";
  const v = validateCosts(s);
  assert.ok(v.ok);
  if (v.ok) {
    const ids = v.sheet.units.civilian.map((l) => l.id);
    assert.equal(new Set(ids).size, ids.length, ids.join(","));
  }
});

test("inherited object keys are not a valid basis", () => {
  for (const basis of ["toString", "constructor", "__proto__", "hasOwnProperty"]) {
    const s = structuredClone(DEFAULT_COSTS) as unknown as { project: Record<string, unknown>[] };
    s.project[0].basis = basis;
    assert.equal(validateCosts(s).ok, false, basis);
  }
});

test("amounts and percentages must really be numbers", () => {
  for (const bad of [null, "", "  ", true, [], {}]) {
    const s = structuredClone(DEFAULT_COSTS) as unknown as { markupPct: unknown; units: { civilian: { amount: unknown }[] } };
    s.units.civilian[0].amount = bad;
    assert.equal(validateCosts(s).ok, false, `amount ${JSON.stringify(bad)}`);
    const t = structuredClone(DEFAULT_COSTS) as unknown as { markupPct: unknown };
    t.markupPct = bad;
    assert.equal(validateCosts(t).ok, false, `markup ${JSON.stringify(bad)}`);
  }
  const s = structuredClone(DEFAULT_COSTS) as unknown as { units: { civilian: { amount: unknown }[] } };
  s.units.civilian[0].amount = "2417";
  assert.equal(validateCosts(s).ok, true, "numeric strings are fine");
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
