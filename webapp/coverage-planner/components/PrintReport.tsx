"use client";
/** The printed report: hidden on screen, laid out under the map when printing. */
import { useEffect, useState } from "react";
import { formatInr, type PublicCosts } from "@/lib/costs";
import { demandPending, householdUnits, households, type PlanState } from "@/lib/planState";
import { ENVIRONMENTS, SENSITIVITY_DBM, UNIT_LABELS, legalTxDbm, type UnitKind } from "@/lib/radio";
import type { Analysis } from "./analysis";
import { findings } from "./CoveragePanel";
import { EstimateTable, planEstimate, priceSource } from "./CostPanel";
import { fmtInt, fmtM, fmtPct } from "./ui";

export default function PrintReport({ plan, analysis: a, costs }: { plan: PlanState; analysis: Analysis | null; costs: PublicCosts | null }) {
  const s = a?.coverage?.stats;
  const km2 = a?.areaKm2 ?? 0;
  const powered = plan.units.filter((u) => u.kind === "powered").length;
  const responders = plan.units.filter((u) => u.kind === "responder").length;
  const env = ENVIRONMENTS.find((e) => e.id === plan.envId);
  const title = plan.report.title || `FloodMesh deployment plan: ${plan.area?.name ?? "area"}`;
  const est = costs ? planEstimate(plan, a, costs) : null;
  const m = plan.model;
  // The date is set in the browser, not in the page built on the server
  // (which would print the build day), and refreshed just before printing.
  const [today, setToday] = useState("");
  useEffect(() => {
    const stamp = () => setToday(new Date().toLocaleDateString("en-IN", { day: "numeric", month: "long", year: "numeric" }));
    stamp();
    window.addEventListener("beforeprint", stamp);
    return () => window.removeEventListener("beforeprint", stamp);
  }, []);
  const pending = demandPending(plan.demand);
  return (
    <div className="printReport">
      <h1>{title}</h1>
      <p className="muted">
        {plan.report.preparedFor && <>Prepared for {plan.report.preparedFor}. </>}
        {plan.report.preparedBy && <>Prepared by {plan.report.preparedBy}. </>}
        {today && <>{today}.</>}
      </p>
      {plan.report.notes && <p className="notes">{plan.report.notes}</p>}

      <h2>Summary</h2>
      <table className="table">
        <tbody>
          <tr><td>Area</td><td>{plan.area?.name ?? "–"}, {km2.toFixed(2)} km²</td></tr>
          <tr>
            <td>Households (estimate)</td>
            <td>
              {pending
                ? "not counted yet (OSM building count pending)"
                : `${fmtInt(households(plan.demand, km2))} (${plan.demand.mode === "osm" ? "from OSM buildings" : plan.demand.mode === "manual" ? "entered" : `${fmtInt(plan.demand.householdsPerKm2)} per km²`}); ${plan.demand.adoptionPct}% get a unit`}
            </td>
          </tr>
          <tr><td>Household units</td><td>{pending ? "–" : fmtInt(householdUnits(plan.demand, km2))}</td></tr>
          <tr><td>Powered units on terraces</td><td>{powered}</td></tr>
          <tr><td>Responder units</td><td>{responders + plan.extraResponders} ({responders} placed on the map)</td></tr>
          {s && <tr><td>Area hearing at least one / two powered units</td><td>{fmtPct(s.pctCovered1)} / {fmtPct(s.pctCovered2)}</td></tr>}
          {s && s.hasResponders && <tr><td>Area whose SOS reaches a responder within {m.hopLimit} relays</td><td>{fmtPct(s.pctSosReachable)}</td></tr>}
          {est && (
            <tr>
              <td>Estimated cost</td>
              <td>
                <strong>{formatInr(est.total)}</strong> (incl. {costs!.contingencyPct}% contingency). {priceSource(costs!)}
              </td>
            </tr>
          )}
        </tbody>
      </table>

      {a && (
        <>
          <h2>Checks</h2>
          <ul className="findings">
            {findings(plan, a).map((f, i) => (
              <li key={i} className={f.tone}>{f.text}</li>
            ))}
          </ul>
        </>
      )}

      {est && (
        <>
          <h2>Cost estimate</h2>
          <EstimateTable est={est} />
          <p className="small">{priceSource(costs!)}</p>
        </>
      )}

      <h2>Radio assumptions</h2>
      <table className="table compact">
        <tbody>
          <tr><td>Band, spreading factor</td><td>865–867 MHz, 125 kHz, SF{m.sf} (sensitivity {SENSITIVITY_DBM[m.sf]} dBm)</td></tr>
          <tr><td>Surroundings</td><td>{env ? env.label : "Custom"}, clutter {m.clutterDb} dB, path-loss exponent {m.pathLossExponent}</td></tr>
          <tr><td>Fade margin, hop limit</td><td>{m.fadeMarginDb} dB, {m.hopLimit} relays</td></tr>
          <tr><td>Duty cycle, e.r.p. ceiling</td><td>{m.dutyCyclePct}% per device, {m.maxErpDbm} dBm (G.S.R. 853(E) as assumed; not yet checked against the gazette)</td></tr>
          {(["civilian", "powered", "responder"] as UnitKind[]).map((k) => {
            const p = plan.profiles[k];
            return (
              <tr key={k}>
                <td>{UNIT_LABELS[k]}</td>
                <td>
                  {p.heightM} m up, {p.antennaDbi} dBi, {p.indoorLossDb ? `${p.indoorLossDb} dB walls, ` : "outdoors, "}
                  {legalTxDbm(p, m).toFixed(0)} dBm
                </td>
              </tr>
            );
          })}
          {a && <tr><td>Resulting ranges</td><td>household ↔ powered {fmtM(a.ranges.householdPowered)}, powered ↔ powered {fmtM(a.ranges.poweredPowered)}, household ↔ household {fmtM(a.ranges.householdHousehold)}</td></tr>}
        </tbody>
      </table>
      <p className="small">
        Method: log-distance path loss fitted to FloodMesh field tests (26 and 28 Sep 2026) with two-ray height gain; coverage on a grid of
        {a?.grid ? ` ${a.grid.cellM} m` : ""} cells, each treated as a household unit indoors. This is a planning estimate with no terrain or
        building data; a walk test at each powered site confirms it. Map data © OpenStreetMap contributors. Constituency boundaries: DataMeet
        India (CC BY 2.5 IN), approximate.
      </p>
    </div>
  );
}
