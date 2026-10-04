/**
 * The built-in cost sheet, used until the admin saves one. Server only (and
 * tests): keeping it out of client components keeps the breakdown out of the
 * public JavaScript.
 *
 * Sources: the electronics line is the landed cost per unit from the priced
 * PCB V1 BoM (docs/hardware/pcb-v1/flood-mesh-v1.0-bom-priced.xlsx, 15-unit
 * build, 27 Sep 2026), the whip antenna is the docs/architecture.md §14.1
 * candidate part, WPC type approval is from docs/certification-india.md.
 * Every other amount is a placeholder estimate to be replaced with quotes.
 */
import type { CostLine, CostSheet } from "./costs.ts";

const base = (): CostLine[] => [
  {
    id: "electronics",
    name: "Electronics, battery and case parts (PCB V1 priced BoM, landed)",
    amount: 2417,
    note: "BoM: 15-unit build, 27 Sep 2026",
  },
  { id: "pcb", name: "PCB fabrication and assembly", amount: 450, note: "Estimate" },
  { id: "enclosure", name: "IP67 enclosure", amount: 450, note: "Estimate" },
  { id: "pigtail", name: "U.FL to SMA bulkhead pigtail", amount: 150, note: "Estimate" },
  { id: "keypad", name: "4×4 membrane keypad", amount: 60, note: "Estimate" },
  { id: "label", name: "Label with call sign and QR code, packaging", amount: 40, note: "Estimate" },
];

export const DEFAULT_COSTS: CostSheet = {
  version: 1,
  currency: "INR",
  markupPct: 0,
  contingencyPct: 10,
  units: {
    civilian: [
      ...base(),
      { id: "whip", name: "868 MHz half-wave whip antenna (LWC-868-RD-RA-SMA)", amount: 307, note: "§14.1 candidate" },
    ],
    powered: [
      ...base(),
      { id: "omni", name: "Outdoor omni antenna, 5–6 dBi fibreglass", amount: 2500, note: "Estimate" },
      { id: "coax", name: "Low-loss coax (LMR-240, 2 m) and connectors", amount: 600, note: "Estimate" },
      { id: "surge", name: "Surge arrestor and earthing", amount: 1200, note: "Estimate" },
      { id: "solar", name: "Solar panel 20 W, charge controller and backup battery", amount: 3500, note: "Estimate" },
      { id: "mount", name: "Pole or wall mount", amount: 800, note: "Estimate" },
      { id: "install", name: "Terrace installation labour", amount: 1500, note: "Estimate" },
    ],
    responder: [
      ...base(),
      { id: "whip", name: "868 MHz half-wave whip antenna (LWC-868-RD-RA-SMA)", amount: 307, note: "§14.1 candidate" },
      { id: "powerbank", name: "Power bank, 10,000 mAh", amount: 900, note: "Estimate" },
      { id: "pouch", name: "Waterproof pouch and lanyard", amount: 250, note: "Estimate" },
    ],
  },
  project: [
    { id: "survey", name: "Site survey and walk test", basis: "per_km2", amount: 3000, note: "Estimate", markup: true },
    {
      id: "calibration",
      name: "Field calibration test (2 days, team of 4)",
      basis: "fixed",
      amount: 20000,
      note: "Estimate",
      markup: true,
    },
    {
      id: "wpc",
      name: "WPC equipment type approval (per hardware model)",
      basis: "fixed",
      amount: 10000,
      note: "docs/certification-india.md",
      // A public fee: marking it up would reveal the markup.
      markup: false,
    },
    {
      id: "handover",
      name: "Household handover and registry entry",
      basis: "per_household_unit",
      amount: 50,
      note: "Estimate",
      markup: true,
    },
    { id: "training", name: "Responder training", basis: "per_responder_unit", amount: 1500, note: "Estimate", markup: true },
    {
      id: "maintenance",
      name: "Year-1 maintenance and battery swaps",
      basis: "per_powered_unit",
      amount: 600,
      note: "Estimate",
      markup: true,
    },
  ],
};
