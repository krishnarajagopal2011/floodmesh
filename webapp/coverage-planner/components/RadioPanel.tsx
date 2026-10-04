"use client";
/** Radio assumptions: spreading factor, environment, unit profiles, calibration, and the method. */
import { useState } from "react";
import {
  DEFAULT_MODEL,
  DEFAULT_PROFILES,
  ENVIRONMENTS,
  FRAME_BYTES,
  FSPL_1M_DB,
  SENSITIVITY_DBM,
  UNIT_LABELS,
  exceedsErp,
  fitExponent,
  frameAirtimeS,
  legalTxDbm,
  type RadioModel,
  type UnitKind,
  type UnitProfile,
} from "@/lib/radio";
import type { PlanState } from "@/lib/planState";
import type { Analysis } from "./analysis";
import { NumInput, NumberField, Section, fmtM } from "./ui";

interface Props {
  plan: PlanState;
  analysis: Analysis | null;
  setModel: (m: Partial<RadioModel>) => void;
  setEnv: (id: string) => void;
  setProfile: (k: UnitKind, p: Partial<UnitProfile>) => void;
  resetRadio: () => void;
}

const KINDS: UnitKind[] = ["civilian", "powered", "responder"];
const PROFILE_FIELDS: { key: keyof UnitProfile; label: string; unit: string; step: number; min: number; max: number }[] = [
  { key: "heightM", label: "Height", unit: "m", step: 0.5, min: 0.5, max: 200 },
  { key: "antennaDbi", label: "Antenna", unit: "dBi", step: 0.1, min: -10, max: 20 },
  { key: "cableLossDb", label: "Cable loss", unit: "dB", step: 0.1, min: 0, max: 20 },
  { key: "indoorLossDb", label: "Wall loss", unit: "dB", step: 1, min: 0, max: 60 },
  { key: "txDbm", label: "TX power", unit: "dBm", step: 1, min: 0, max: 30 },
];

export default function RadioPanel({ plan, analysis, setModel, setEnv, setProfile, resetRadio }: Props) {
  const m = plan.model;
  const [cal, setCal] = useState({ distanceM: 350, rssiDbm: -122, txH: 1, rxH: 1, gain: 0 });
  const calN = fitExponent(
    cal.distanceM,
    cal.rssiDbm,
    { heightM: cal.txH, antennaDbi: cal.gain, cableLossDb: 0, indoorLossDb: 0, txDbm: 20 },
    { heightM: cal.rxH, antennaDbi: cal.gain, cableLossDb: 0, indoorLossDb: 0, txDbm: 20 },
    { ...m, clutterDb: 0 },
  );
  const r = analysis?.ranges;

  return (
    <div>
      <Section title="Network settings">
        <div className="field">
          <label htmlFor="sf">Spreading factor (one for the whole network, §13.7)</label>
          <select id="sf" value={m.sf} onChange={(e) => setModel({ sf: Number(e.target.value) })}>
            {Object.keys(SENSITIVITY_DBM).map((sf) => (
              <option key={sf} value={sf}>
                SF{sf} · sensitivity {SENSITIVITY_DBM[Number(sf)]} dBm · SOS {(frameAirtimeS(FRAME_BYTES.sos, { ...m, sf: Number(sf) }) * 1000).toFixed(0)} ms
              </option>
            ))}
          </select>
        </div>
        <div className="field">
          <label htmlFor="env">Surroundings</label>
          <select id="env" value={plan.envId} onChange={(e) => setEnv(e.target.value)}>
            {ENVIRONMENTS.map((e) => (
              <option key={e.id} value={e.id}>
                {e.label} ({e.clutterDb >= 0 ? "+" : ""}
                {e.clutterDb} dB)
              </option>
            ))}
            <option value="custom">Custom</option>
          </select>
        </div>
        <div className="grid2">
          <NumberField label="Extra clutter loss" value={m.clutterDb} onChange={(v) => setModel({ clutterDb: v })} min={-20} max={40} step={0.5} unit="dB" />
          <NumberField label="Path-loss exponent n" value={m.pathLossExponent} onChange={(v) => setModel({ pathLossExponent: v })} min={2} max={6} step={0.05} />
          <NumberField label="Fade margin" value={m.fadeMarginDb} onChange={(v) => setModel({ fadeMarginDb: v })} min={0} max={40} step={1} unit="dB" />
          <NumberField label="Height gain cap (each end)" value={m.heightGainCapDb} onChange={(v) => setModel({ heightGainCapDb: v })} min={0} max={40} step={1} unit="dB" />
          <NumberField label="Hop limit (FM_HOP_DEFAULT)" value={m.hopLimit} onChange={(v) => setModel({ hopLimit: Math.round(v) })} min={1} max={7} step={1} />
          <NumberField label="Wake-up preamble" value={m.wakePreambleS} onChange={(v) => setModel({ wakePreambleS: v })} min={0} max={5} step={0.1} unit="s" />
          <NumberField label="Duty cycle limit" value={m.dutyCyclePct} onChange={(v) => setModel({ dutyCyclePct: v })} min={0.1} max={100} step={0.1} unit="%" />
          <NumberField label="Legal e.r.p. ceiling" value={m.maxErpDbm} onChange={(v) => setModel({ maxErpDbm: v })} min={0} max={40} step={0.5} unit="dBm" />
          <NumberField label="Heartbeats per hour" value={m.heartbeatsPerHour} onChange={(v) => setModel({ heartbeatsPerHour: v })} min={0} max={60} step={1} />
        </div>
      </Section>

      <Section title="Unit profiles">
        <div className="tableWrap">
          <table className="table compact profiles">
            <thead>
              <tr>
                <th></th>
                {KINDS.map((k) => (
                  <th key={k}>{UNIT_LABELS[k]}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {PROFILE_FIELDS.map((f) => (
                <tr key={f.key}>
                  <th>
                    {f.label} <span className="muted">({f.unit})</span>
                  </th>
                  {KINDS.map((k) => (
                    <td key={k}>
                      <NumInput
                        ariaLabel={`${UNIT_LABELS[k]} ${f.label}`}
                        value={plan.profiles[k][f.key]}
                        step={f.step}
                        min={f.min}
                        max={f.max}
                        onChange={(v) => setProfile(k, { [f.key]: v })}
                      />
                    </td>
                  ))}
                </tr>
              ))}
              <tr>
                <th>Legal TX</th>
                {KINDS.map((k) => (
                  <td key={k} className={exceedsErp(plan.profiles[k], m) ? "warnText" : ""}>
                    {legalTxDbm(plan.profiles[k], m).toFixed(1)} dBm
                  </td>
                ))}
              </tr>
            </tbody>
          </table>
        </div>
        <p className="hint">
          Legal TX = min(TX, e.r.p. ceiling + 2.15 − antenna + cable loss), §14.2 rule 2. Placed units can override height and antenna one by
          one (Units tab).
        </p>
      </Section>

      <Section title="Resulting ranges">
        {r && (
          <table className="table compact">
            <tbody>
              <tr><td>Household (indoors) ↔ powered unit</td><td className="num">{fmtM(r.householdPowered)}</td></tr>
              <tr><td>Powered ↔ powered (backbone)</td><td className="num">{fmtM(r.poweredPowered)}</td></tr>
              <tr><td>Responder ↔ powered unit</td><td className="num">{fmtM(r.responderPowered)}</td></tr>
              <tr><td>Household ↔ responder</td><td className="num">{fmtM(r.householdResponder)}</td></tr>
              <tr><td>Household ↔ household (battery mesh)</td><td className="num">{fmtM(r.householdHousehold)}</td></tr>
            </tbody>
          </table>
        )}
        <p className="hint">The weaker direction of each link counts: an SOS goes one way and its ACK comes back.</p>
      </Section>

      <Section title="Calibrate from a field reading">
        <p className="hint">Enter one reading from a walk test between two units outdoors to fit n. Defaults: 26 Sep test, 350 m at −122 dBm (stock antennas taken as 0 dBi, both about 1 m up).</p>
        <div className="grid2">
          <NumberField label="Distance" value={cal.distanceM} onChange={(v) => setCal({ ...cal, distanceM: v })} min={10} max={30000} unit="m" />
          <NumberField label="Signal received" value={cal.rssiDbm} onChange={(v) => setCal({ ...cal, rssiDbm: v })} min={-150} max={0} unit="dBm" />
          <NumberField label="Sender height" value={cal.txH} onChange={(v) => setCal({ ...cal, txH: v })} min={0.5} max={200} step={0.5} unit="m" />
          <NumberField label="Receiver height" value={cal.rxH} onChange={(v) => setCal({ ...cal, rxH: v })} min={0.5} max={200} step={0.5} unit="m" />
          <NumberField label="Antenna gain (each)" value={cal.gain} onChange={(v) => setCal({ ...cal, gain: v })} min={-10} max={20} step={0.1} unit="dBi" />
        </div>
        <div className="row">
          <span>
            Fitted n = <strong>{Number.isFinite(calN) ? calN.toFixed(2) : "–"}</strong> (at 20 dBm, for the calibrated surroundings)
          </span>
          <button
            type="button"
            className="secondary"
            disabled={!Number.isFinite(calN) || calN < 2 || calN > 6}
            onClick={() => setModel({ pathLossExponent: Math.round(calN * 100) / 100 })}
          >
            Use it
          </button>
        </div>
      </Section>

      <Section title="Method">
        <details>
          <summary>How coverage is computed</summary>
          <ol className="method">
            <li>
              <strong>Path loss</strong> (log-distance, fitted to the FloodMesh field tests): PL(d) = {FSPL_1M_DB.toFixed(1)} + 10·n·log₁₀(d) − G<sub>h</sub>(h₁) − G<sub>h</sub>(h₂) + clutter, never less
              than free space. G<sub>h</sub>(h) = min(20·log₁₀(h / 1 m), cap): the two-ray height gain, which matches the field-test estimate of +15 dB at 6 m and +20 dB at 10 m.
            </li>
            <li>
              <strong>Link budget</strong>: largest path loss = TX + G<sub>tx</sub> + G<sub>rx</sub> − cable and wall losses − sensitivity(SF) − fade margin. The weaker
              direction counts. A link works where PL(d) ≤ that budget; the range is where they are equal.
            </li>
            <li>
              <strong>Coverage map</strong>: the area is cut into cells (10–500 m, about 40,000 cells). Each cell is a household unit indoors; it counts the powered
              units it can reach. Green: 2 or more, amber: 1, red: none.
            </li>
            <li>
              <strong>SOS reach</strong>: links between placed units form the backbone. Each powered unit gets the number of relays to the nearest responder
              (breadth-first search); a cell reaches a responder if one of its powered units does so within the hop limit. Battery household units also
              forward SOS (§13.3), so real reach is better than shown.
            </li>
            <li>
              <strong>Airtime</strong>: LoRa time on air (Semtech AN1200.13, 125 kHz, CR 4/5, 16-symbol preamble) plus the radio-wake preamble. The heartbeat
              load is an upper bound: every powered unit within the hop limit relays every heartbeat, with no race cancellation (§13.3).
            </li>
            <li>
              <strong>Battery-only fallback</strong>: if every powered unit fails, household units still form one mesh when each hears about 4.5 others on
              average (continuum percolation threshold).
            </li>
          </ol>
          <p className="hint">
            Limits: no terrain or building database, no flood water, no interference from other 865–867 MHz users. A walk test at each powered site
            (docs/field-tests.md) is what turns this estimate into a plan.
          </p>
        </details>
      </Section>

      <div className="row">
        <button type="button" className="secondary" onClick={resetRadio}>
          Reset radio settings to defaults
        </button>
      </div>
      <p className="hint">
        Defaults: SF{DEFAULT_MODEL.sf}, n = {DEFAULT_MODEL.pathLossExponent}, fade margin {DEFAULT_MODEL.fadeMarginDb} dB, household unit {DEFAULT_PROFILES.civilian.indoorLossDb} dB
        indoors, powered unit {DEFAULT_PROFILES.powered.heightM} m up with a {DEFAULT_PROFILES.powered.antennaDbi} dBi omni.
      </p>
    </div>
  );
}
