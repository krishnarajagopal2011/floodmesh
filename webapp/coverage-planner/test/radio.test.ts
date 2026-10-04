import { test } from "node:test";
import assert from "node:assert/strict";
import {
  DEFAULT_MODEL,
  DEFAULT_PROFILES,
  airtimeS,
  fitExponent,
  heightGainDb,
  legalTxDbm,
  marginDb,
  rangeM,
  type UnitProfile,
} from "../lib/radio.ts";

const near = (a: number, b: number, tol: number, msg?: string) => assert.ok(Math.abs(a - b) <= tol, msg ?? `${a} not within ${tol} of ${b}`);

test("airtime matches docs/architecture.md §1.2", () => {
  near(airtimeS(10, 7) * 1000, 49, 1, "10 B at SF7");
  near(airtimeS(22, 7) * 1000, 65, 1, "22 B at SF7");
  near(airtimeS(22, 9) * 1000, 239, 1, "22 B at SF9");
  near(airtimeS(32, 10) * 1000, 518, 1, "32 B at SF10");
  near(airtimeS(22, 10) * 1000, 436, 1, "22 B at SF10");
});

test("height gain matches the field-test estimate (+15 dB at 6 m, +20 dB at 10 m)", () => {
  near(heightGainDb(6, 20), 15.6, 0.1);
  near(heightGainDb(10, 20), 20, 0.01);
  assert.equal(heightGainDb(30, 20), 20, "capped");
  assert.equal(heightGainDb(0.5, 20), 0, "never negative");
});

test("the 26 Sep field test fits n ≈ 4.36", () => {
  const stock: UnitProfile = { heightM: 1, antennaDbi: 0, cableLossDb: 0, indoorLossDb: 0, txDbm: 20 };
  near(fitExponent(350, -122, stock, stock, { ...DEFAULT_MODEL, clutterDb: 0 }), 4.36, 0.01);
});

test("range is where the margin reaches zero", () => {
  const m = DEFAULT_MODEL;
  for (const [a, b] of [
    ["civilian", "powered"],
    ["powered", "powered"],
    ["civilian", "civilian"],
    ["responder", "powered"],
  ] as const) {
    const r = rangeM(DEFAULT_PROFILES[a], DEFAULT_PROFILES[b], m);
    assert.ok(r > 10 && r < 30_000, `${a}-${b} range ${r}`);
    near(marginDb(r, DEFAULT_PROFILES[a], DEFAULT_PROFILES[b], m), 0, 1e-6);
  }
  // Raising the powered unit or improving SF must extend the range.
  const base = rangeM(DEFAULT_PROFILES.civilian, DEFAULT_PROFILES.powered, m);
  assert.ok(rangeM(DEFAULT_PROFILES.civilian, { ...DEFAULT_PROFILES.powered, heightM: 3 }, m) < base);
  assert.ok(rangeM(DEFAULT_PROFILES.civilian, DEFAULT_PROFILES.powered, { ...m, sf: 9 }) > base);
});

test("legal TX follows §14.2 rule 2 (12 dBi Yagi, 1 dB cable ≈ 18 dBm)", () => {
  const yagi: UnitProfile = { heightM: 10, antennaDbi: 12, cableLossDb: 1, indoorLossDb: 0, txDbm: 20 };
  near(legalTxDbm(yagi, DEFAULT_MODEL), 18.15, 0.01);
  assert.equal(legalTxDbm(DEFAULT_PROFILES.civilian, DEFAULT_MODEL), 20);
});
