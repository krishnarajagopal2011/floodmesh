"use client";
/** Cost estimate from the admin's price list and the plan's quantities. */
import { COST_BASES, estimate, formatInr, type Estimate, type PublicCosts } from "@/lib/costs";
import { householdUnits, households, type PlanState } from "@/lib/planState";
import type { Analysis } from "./analysis";
import { Section, Stat, fmtInt } from "./ui";

export function planEstimate(plan: PlanState, a: Analysis | null, costs: PublicCosts): Estimate {
  const km2 = a?.areaKm2 ?? 0;
  return estimate(costs, {
    civilian: householdUnits(plan.demand, km2),
    powered: plan.units.filter((u) => u.kind === "powered").length,
    responder: plan.units.filter((u) => u.kind === "responder").length + plan.extraResponders,
    areaKm2: km2,
  });
}

export function EstimateTable({ est }: { est: Estimate }) {
  return (
    <table className="table compact costTable">
      <thead>
        <tr>
          <th>Item</th>
          <th className="num">Qty</th>
          <th className="num">Rate</th>
          <th className="num">Amount</th>
        </tr>
      </thead>
      <tbody>
        {est.lines.map((l, i) => (
          <tr key={i}>
            <td>{l.name}</td>
            <td className="num">
              {l.unit === "lump sum" ? "1" : l.unit === "km²" ? l.quantity.toFixed(2) : fmtInt(l.quantity)}
            </td>
            <td className="num">{formatInr(l.rate)}</td>
            <td className="num">{formatInr(l.total)}</td>
          </tr>
        ))}
      </tbody>
      <tfoot>
        <tr>
          <td colSpan={3}>Subtotal</td>
          <td className="num">{formatInr(est.subtotal)}</td>
        </tr>
        <tr>
          <td colSpan={3}>Contingency</td>
          <td className="num">{formatInr(est.contingency)}</td>
        </tr>
        <tr className="total">
          <td colSpan={3}>Total</td>
          <td className="num">{formatInr(est.total)}</td>
        </tr>
      </tfoot>
    </table>
  );
}

export default function CostPanel({
  plan,
  analysis,
  costs,
  costsError,
}: {
  plan: PlanState;
  analysis: Analysis | null;
  costs: PublicCosts | null;
  costsError: string | null;
}) {
  if (!costs) return <p className={costsError ? "badText" : "muted"}>{costsError ?? "Loading prices…"}</p>;
  const est = planEstimate(plan, analysis, costs);
  const km2 = analysis?.areaKm2 ?? 0;
  const hh = households(plan.demand, km2);
  const hhu = householdUnits(plan.demand, km2);
  return (
    <div>
      <div className="stats">
        <Stat label="Total estimate" value={formatInr(est.total)} sub={`incl. ${costs.contingencyPct}% contingency`} />
        <Stat label="Per household unit" value={hhu ? formatInr(est.total / hhu) : "–"} sub={`${fmtInt(hhu)} units`} />
        <Stat label="Per household in area" value={hh ? formatInr(est.total / hh) : "–"} sub={`${fmtInt(hh)} households`} />
        <Stat label="Per km²" value={km2 ? formatInr(est.total / km2) : "–"} sub={`${km2.toFixed(2)} km²`} />
      </div>
      <Section title="Estimate">
        <EstimateTable est={est} />
      </Section>
      <Section title="Prices used">
        <table className="table compact">
          <tbody>
            <tr>
              <td>Household unit</td>
              <td className="num">{formatInr(costs.unitPrice.civilian)}</td>
            </tr>
            <tr>
              <td>Powered unit, installed on a terrace</td>
              <td className="num">{formatInr(costs.unitPrice.powered)}</td>
            </tr>
            <tr>
              <td>Responder unit</td>
              <td className="num">{formatInr(costs.unitPrice.responder)}</td>
            </tr>
            {costs.project.map((l) => (
              <tr key={l.id}>
                <td>
                  {l.name} <span className="muted">({COST_BASES[l.basis].toLowerCase()})</span>
                </td>
                <td className="num">{formatInr(l.rate)}</td>
              </tr>
            ))}
          </tbody>
        </table>
        <p className="hint">
          {costs.isDefault
            ? "Built-in placeholder prices: the admin has not set a price list yet."
            : `Price list set by the project admin, last changed ${new Date(costs.updatedAt!).toLocaleDateString("en-IN", { day: "numeric", month: "short", year: "numeric" })}.`}{" "}
          Quantities come from the Units tab.
        </p>
      </Section>
    </div>
  );
}
