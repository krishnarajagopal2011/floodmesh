/**
 * Radio model for coverage planning: link budget, path loss, range and
 * airtime. Pure functions, shared by the browser and the tests.
 *
 * The path-loss model is a log-distance model fitted to the FloodMesh field
 * tests of 26 and 28 Sep 2026 (docs/field-tests.md): units about 1 m above a
 * road, 866.5 MHz, SF7, 20 dBm, stock antennas.
 *
 *   PL(d) = FSPL(1 m) + 10·n·log10(d / 1 m) − Gh(h1) − Gh(h2) + clutter
 *   Gh(h) = min(20·log10(h / 1 m), cap)            (two-ray height gain)
 *
 * The fit: 350 m at −122 dBm, 300 m at −112 dBm and 250 m at −114 dBm give
 * n = 4.36, 4.07 and 4.29; the default 4.2 is their middle. The height gain
 * reproduces the field-test estimate of +15 dB for 6 m and +20 dB for 10 m.
 * PL never goes below free-space loss.
 *
 * It is a planning estimate, not a survey: a building, a tree line or a
 * flooded street can cost 10–20 dB within metres. The fade margin covers
 * part of that; a walk test at each powered site settles the rest.
 */

export const FREQ_MHZ = 866.5;
const C = 299_792_458;
export const WAVELENGTH_M = C / (FREQ_MHZ * 1e6);
/** Free-space loss at 1 m: 20·log10(4π/λ) ≈ 31.2 dB. */
export const FSPL_1M_DB = 20 * Math.log10((4 * Math.PI) / WAVELENGTH_M);

/** SX1262 sensitivity at 125 kHz, as used in docs/architecture.md §1.2 (≈2.5 dB per SF step). */
export const SENSITIVITY_DBM: Readonly<Record<number, number>> = {
  7: -124,
  8: -126.5,
  9: -129,
  10: -131.5,
  11: -134,
  12: -136.5,
};

/** Frame sizes in bytes (firmware_v4/include/fm_packet.h). */
export const FRAME_BYTES = { sos: 22, ack: 28, heartbeat: 23, text: 67 } as const;

export type UnitKind = "civilian" | "powered" | "responder";

/** How one kind of unit sits in the world: height, antenna, losses, power. */
export interface UnitProfile {
  heightM: number;
  antennaDbi: number;
  cableLossDb: number;
  /** Walls between the antenna and the street (0 outdoors). */
  indoorLossDb: number;
  txDbm: number;
}

export interface RadioModel {
  sf: number;
  pathLossExponent: number;
  /** Extra loss for the surroundings relative to the field-test site. */
  clutterDb: number;
  fadeMarginDb: number;
  heightGainCapDb: number;
  /** Frame hop limit: how many units may relay an SOS (FM_HOP_DEFAULT = 3). */
  hopLimit: number;
  /** Radio-wake preamble added to every frame sleeping units must hear (§13.1). */
  wakePreambleS: number;
  /** Licence-exempt duty cycle per device, % (G.S.R. 853(E) as assumed in §1.1). */
  dutyCyclePct: number;
  /** Legal e.r.p. ceiling in dBm (500 mW = 27 dBm, §1.1, unverified). */
  maxErpDbm: number;
  /** Heartbeats per hour from each powered unit (§13.5: every 30 min). */
  heartbeatsPerHour: number;
}

export interface Environment {
  id: string;
  label: string;
  clutterDb: number;
}

export const ENVIRONMENTS: readonly Environment[] = [
  { id: "dense", label: "Dense urban (old city core, 3–5 storey blocks)", clutterDb: 6 },
  { id: "urban", label: "Urban (mixed housing, main roads)", clutterDb: 3 },
  { id: "suburban", label: "Suburban (field-test site, calibrated)", clutterDb: 0 },
  { id: "open", label: "Open (fields, water, wide roads)", clutterDb: -6 },
];

export const DEFAULT_MODEL: RadioModel = {
  sf: 7,
  pathLossExponent: 4.2,
  clutterDb: 3,
  fadeMarginDb: 10,
  heightGainCapDb: 20,
  hopLimit: 3,
  wakePreambleS: 0.5,
  dutyCyclePct: 2.5,
  maxErpDbm: 27,
  heartbeatsPerHour: 2,
};

export const DEFAULT_PROFILES: Readonly<Record<UnitKind, UnitProfile>> = {
  // Household unit indoors on the ground floor, standard half-wave whip (§14.1).
  civilian: { heightM: 1.5, antennaDbi: 2.2, cableLossDb: 0.5, indoorLossDb: 12, txDbm: 20 },
  // Unit on external power on a terrace: 5–6 dBi outdoor omni, short coax (§14.2).
  powered: { heightM: 10, antennaDbi: 6, cableLossDb: 1, indoorLossDb: 0, txDbm: 20 },
  // Responder handheld outdoors or on a boat.
  responder: { heightM: 1.5, antennaDbi: 2.2, cableLossDb: 0.5, indoorLossDb: 0, txDbm: 20 },
};

export const UNIT_LABELS: Readonly<Record<UnitKind, string>> = {
  civilian: "Household unit",
  powered: "Powered unit (terrace)",
  responder: "Responder unit",
};

export function fsplDb(distanceM: number): number {
  return FSPL_1M_DB + 20 * Math.log10(Math.max(distanceM, 1));
}

export function heightGainDb(heightM: number, capDb: number): number {
  return Math.min(Math.max(20 * Math.log10(Math.max(heightM, 1)), 0), capDb);
}

export function pathLossDb(distanceM: number, h1: number, h2: number, m: RadioModel): number {
  const d = Math.max(distanceM, 1);
  const pl =
    FSPL_1M_DB +
    10 * m.pathLossExponent * Math.log10(d) -
    heightGainDb(h1, m.heightGainCapDb) -
    heightGainDb(h2, m.heightGainCapDb) +
    m.clutterDb;
  return Math.max(pl, fsplDb(d));
}

/**
 * Transmit power after the legal e.r.p. ceiling: e.i.r.p. = e.r.p. + 2.15 dB,
 * so tx ≤ maxErp + 2.15 − antenna gain + cable loss (§14.2 rule 2).
 */
export function legalTxDbm(p: UnitProfile, m: RadioModel): number {
  return Math.min(p.txDbm, m.maxErpDbm + 2.15 - p.antennaDbi + p.cableLossDb);
}

/** True when the profile's own transmit power would break the e.r.p. ceiling. */
export function exceedsErp(p: UnitProfile, m: RadioModel): boolean {
  return legalTxDbm(p, m) < p.txDbm - 1e-9;
}

export interface LinkBudget {
  txDbm: number;
  txGainDb: number;
  rxGainDb: number;
  lossesDb: number;
  sensitivityDbm: number;
  fadeMarginDb: number;
  /** Largest path loss that still closes the link with the fade margin. */
  maxPathLossDb: number;
}

/** One direction a → b. */
export function linkBudget(a: UnitProfile, b: UnitProfile, m: RadioModel): LinkBudget {
  const txDbm = legalTxDbm(a, m);
  const sensitivityDbm = SENSITIVITY_DBM[m.sf] ?? SENSITIVITY_DBM[7];
  const lossesDb = a.cableLossDb + a.indoorLossDb + b.cableLossDb + b.indoorLossDb;
  const maxPathLossDb = txDbm + a.antennaDbi + b.antennaDbi - lossesDb - sensitivityDbm - m.fadeMarginDb;
  return {
    txDbm,
    txGainDb: a.antennaDbi,
    rxGainDb: b.antennaDbi,
    lossesDb,
    sensitivityDbm,
    fadeMarginDb: m.fadeMarginDb,
    maxPathLossDb,
  };
}

/** A link must work both ways (SOS one way, ACK back), so the weaker direction counts. */
export function maxPathLossDb(a: UnitProfile, b: UnitProfile, m: RadioModel): number {
  return Math.min(linkBudget(a, b, m).maxPathLossDb, linkBudget(b, a, m).maxPathLossDb);
}

/** Margin left over the fade margin at distance d, dB. ≥ 0 means the link is expected to work. */
export function marginDb(distanceM: number, a: UnitProfile, b: UnitProfile, m: RadioModel): number {
  return maxPathLossDb(a, b, m) - pathLossDb(distanceM, a.heightM, b.heightM, m);
}

/** Received signal at b from a, dBm (no fade margin). */
export function rssiDbm(distanceM: number, a: UnitProfile, b: UnitProfile, m: RadioModel): number {
  return (
    legalTxDbm(a, m) +
    a.antennaDbi +
    b.antennaDbi -
    (a.cableLossDb + a.indoorLossDb + b.cableLossDb + b.indoorLossDb) -
    pathLossDb(distanceM, a.heightM, b.heightM, m)
  );
}

const MAX_RANGE_M = 30_000;

/** Largest distance at which the link still has a margin ≥ 0. */
export function rangeM(a: UnitProfile, b: UnitProfile, m: RadioModel): number {
  const budget = maxPathLossDb(a, b, m);
  const hg = heightGainDb(a.heightM, m.heightGainCapDb) + heightGainDb(b.heightM, m.heightGainCapDb);
  const logD = (budget - FSPL_1M_DB + hg - m.clutterDb) / (10 * m.pathLossExponent);
  let d = Math.pow(10, logD);
  // Free space is the floor of the model; at short range it can be the binding term.
  const dFree = Math.pow(10, (budget - FSPL_1M_DB) / 20);
  d = Math.min(d, dFree);
  if (!Number.isFinite(d) || budget < FSPL_1M_DB) return 0;
  return Math.min(Math.max(d, 0), MAX_RANGE_M);
}

/**
 * The path-loss exponent that explains one measurement: a received signal at
 * a known distance between two units of known height, power and antennas.
 */
export function fitExponent(
  distanceM: number,
  measuredDbm: number,
  tx: UnitProfile,
  rx: UnitProfile,
  m: RadioModel,
): number {
  const pl =
    tx.txDbm + tx.antennaDbi + rx.antennaDbi - (tx.cableLossDb + tx.indoorLossDb + rx.cableLossDb + rx.indoorLossDb) -
    measuredDbm;
  const hg = heightGainDb(tx.heightM, m.heightGainCapDb) + heightGainDb(rx.heightM, m.heightGainCapDb);
  return (pl - FSPL_1M_DB + hg - m.clutterDb) / (10 * Math.log10(Math.max(distanceM, 2)));
}

/**
 * LoRa time on air (Semtech AN1200.13), explicit header, CRC on, 125 kHz,
 * coding rate 4/5, 16-symbol preamble as in firmware_v4 (fm_radio.h).
 */
export function airtimeS(payloadBytes: number, sf: number, preambleSymbols = 16, bwHz = 125_000, cr = 1): number {
  const tSym = Math.pow(2, sf) / bwHz;
  const de = tSym > 0.016 ? 1 : 0;
  const tPreamble = (preambleSymbols + 4.25) * tSym;
  const num = 8 * payloadBytes - 4 * sf + 28 + 16;
  const nPayload = 8 + Math.max(Math.ceil(num / (4 * (sf - 2 * de))) * (cr + 4), 0);
  return tPreamble + nPayload * tSym;
}

/** Airtime of a frame that sleeping units must hear (adds the wake-up preamble). */
export function frameAirtimeS(payloadBytes: number, m: RadioModel): number {
  return airtimeS(payloadBytes, m.sf) + m.wakePreambleS;
}

/** Seconds of transmission per hour one device may use. */
export function dutyBudgetSPerHour(m: RadioModel): number {
  return 3600 * (m.dutyCyclePct / 100);
}
