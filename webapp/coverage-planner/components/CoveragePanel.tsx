"use client";
/** Coverage results: map layer, headline numbers, network checks and warnings. */
import { PERCOLATION_MEAN_DEGREE, USABLE_CHANNEL_S_PER_HOUR, meanNeighbours } from "@/lib/plan";
import { householdUnits, type PlanState } from "@/lib/planState";
import type { Analysis, CoverageView } from "./analysis";
import { Check, Section, Stat, fmtInt, fmtM, fmtPct } from "./ui";

interface Props {
  plan: PlanState;
  analysis: Analysis | null;
  view: CoverageView;
  setView: (v: CoverageView) => void;
  opacity: number;
  setOpacity: (v: number) => void;
  showLinks: boolean;
  setShowLinks: (v: boolean) => void;
  showRanges: boolean;
  setShowRanges: (v: boolean) => void;
  showHouseholds: boolean;
  setShowHouseholds: (v: boolean) => void;
}

export interface Finding {
  tone: "ok" | "warn" | "bad";
  text: string;
}

/** Plain-language checks on the plan, worst first. Also used by the printed report. */
export function findings(plan: PlanState, a: Analysis | null): Finding[] {
  const out: Finding[] = [];
  if (!a) return out;
  const powered = plan.units.filter((u) => u.kind === "powered").length;
  const responders = plan.units.filter((u) => u.kind === "responder").length;
  const s = a.coverage?.stats;
  if (!plan.area) out.push({ tone: "warn", text: "No planning area chosen yet." });
  if (powered === 0) out.push({ tone: "bad", text: "No powered units placed: household units depend on each other alone." });
  if (s && powered > 0) {
    if (s.pctCovered1 < 95) out.push({ tone: "bad", text: `${fmtPct(100 - s.pctCovered1)} of the area hears no powered unit.` });
    else if (s.pctCovered2 < 90)
      out.push({ tone: "warn", text: `${fmtPct(100 - s.pctCovered2)} of the area hears only one powered unit (target: two, §4.2).` });
    else out.push({ tone: "ok", text: `${fmtPct(s.pctCovered2)} of the area hears two or more powered units.` });
  }
  if (responders === 0 && powered > 0) out.push({ tone: "warn", text: "No responder placed, so SOS reach can't be checked." });
  if (s && responders > 0) {
    if (s.pctSosReachable < 95)
      out.push({
        tone: "bad",
        text: `SOS from ${fmtPct(100 - s.pctSosReachable)} of the area can't reach a responder within ${plan.model.hopLimit} relays. Add responders or raise the hop limit.`,
      });
    else out.push({ tone: "ok", text: `SOS reaches a responder from ${fmtPct(s.pctSosReachable)} of the area within ${plan.model.hopLimit} relays.` });
  }
  const hs = a.households?.stats;
  if (hs && hs.hasResponders && hs.pctSosNoPowered < 50 && powered > 0)
    out.push({
      tone: "warn",
      text: `If every powered unit failed, only ${fmtPct(hs.pctSosNoPowered)} of household units could still reach a responder within ${plan.model.hopLimit} relays. More responders spread over the area, or a higher hop limit, would raise it.`,
    });
  if (a.poweredIslands > 1)
    out.push({ tone: "warn", text: `Powered units form ${a.poweredIslands} separate islands that can't hear each other.` });
  if (a.airtime.heartbeatChannelSPerHour > USABLE_CHANNEL_S_PER_HOUR / 2)
    out.push({
      tone: "bad",
      text: `Heartbeats alone would take ${a.airtime.heartbeatChannelSPerHour.toFixed(0)} s/h of the ~${USABLE_CHANNEL_S_PER_HOUR} s/h usable channel around the busiest powered unit. Use fewer powered units, a lower hop limit or a longer heartbeat period.`,
    });
  else if (a.airtime.worstHeartbeatSPerHour > a.airtime.budgetSPerHour)
    out.push({
      tone: "warn",
      text: `If the forwarding race failed, the busiest powered unit would need ${a.airtime.worstHeartbeatSPerHour.toFixed(0)} s/h to relay heartbeats, over its ${a.airtime.budgetSPerHour.toFixed(0)} s/h duty budget; the firmware should drop heartbeats first (§13.5).`,
    });
  const order = { bad: 0, warn: 1, ok: 2 };
  return out.sort((x, y) => order[x.tone] - order[y.tone]);
}

export function Legend({ view, hopLimit }: { view: CoverageView; hopLimit: number }) {
  const items: [string, string][] =
    view === "redundancy"
      ? [
          ["#149646", "2+ powered units"],
          ["#ebaa00", "1 powered unit"],
          ["#c81e1e", "none"],
        ]
      : view === "signal"
        ? [
            ["#14823c", "> 20 dB margin"],
            ["#6ebe46", "10–20 dB"],
            ["#ebbe00", "5–10 dB"],
            ["#f07814", "0–5 dB"],
            ["#c81e1e", "no link"],
          ]
        : view === "sos"
          ? ([
              ["#146e32", "direct to responder"],
              hopLimit > 1 ? ["#149646", "1 relay"] : null,
              hopLimit > 2 ? ["#82be3c", hopLimit === 3 ? "2 relays" : `2–${hopLimit - 1} relays`] : null,
              ["#ebaa00", `${hopLimit} relay${hopLimit === 1 ? "" : "s"} (limit)`],
              ["#c81e1e", "not reached"],
            ].filter(Boolean) as [string, string][])
          : [];
  return (
    <div className="legend">
      {items.map(([c, t]) => (
        <span key={t}>
          <i style={{ background: c }} /> {t}
        </span>
      ))}
    </div>
  );
}

export default function CoveragePanel(p: Props) {
  const { plan, analysis: a } = p;
  const s = a?.coverage?.stats;
  const km2 = a?.areaKm2 ?? 0;
  const hhu = householdUnits(plan.demand, km2);
  const density = km2 > 0 ? hhu / km2 : 0;
  const nbrs = a ? meanNeighbours(density, a.ranges.householdHousehold) : 0;
  const fs = findings(plan, a);

  return (
    <div>
      <Section title="Map layer">
        <div className="segmented" role="group" aria-label="Coverage layer">
          {(
            [
              ["redundancy", "Powered units heard"],
              ["signal", "Signal margin"],
              ["sos", "SOS reach"],
              ["none", "Off"],
            ] as const
          ).map(([id, label]) => (
            <button key={id} type="button" className={p.view === id ? "on" : ""} onClick={() => p.setView(id)}>
              {label}
            </button>
          ))}
        </div>
        <Legend view={p.view} hopLimit={plan.model.hopLimit} />
        <div className="field">
          <label htmlFor="op">Layer opacity</label>
          <input id="op" type="range" min={0.1} max={0.9} step={0.05} value={p.opacity} onChange={(e) => p.setOpacity(Number(e.target.value))} />
        </div>
        <Check label="Show links between units (shortest-link tree)" checked={p.showLinks} onChange={p.setShowLinks} />
        <Check label="Show each unit's reach to household units" checked={p.showRanges} onChange={p.setShowRanges} />
        {plan.householdPoints && (
          <Check
            label={`Show placed household units (${fmtInt(plan.householdPoints.points.length)} dots, coloured like the layer)`}
            checked={p.showHouseholds}
            onChange={p.setShowHouseholds}
          />
        )}
      </Section>

      {fs.length > 0 && (
        <Section title="Checks">
          <ul className="findings">
            {fs.map((f, i) => (
              <li key={i} className={f.tone}>
                {f.text}
              </li>
            ))}
          </ul>
        </Section>
      )}

      <Section title="Coverage">
        {s ? (
          <div className="stats">
            <Stat label="Area" value={`${km2.toFixed(2)} km²`} sub={`${fmtInt(s.cells)} cells of ${a!.grid!.cellM} m`} />
            <Stat label="Hears ≥ 1 powered" value={fmtPct(s.pctCovered1)} tone={s.pctCovered1 >= 95 ? "ok" : "bad"} />
            <Stat label="Hears ≥ 2 powered" value={fmtPct(s.pctCovered2)} tone={s.pctCovered2 >= 90 ? "ok" : "warn"} />
            <Stat
              label="SOS reaches responder"
              value={s.hasResponders ? fmtPct(s.pctSosReachable) : "–"}
              tone={!s.hasResponders ? undefined : s.pctSosReachable >= 95 ? "ok" : "bad"}
              sub={`within ${plan.model.hopLimit} relays`}
            />
          </div>
        ) : (
          <p className="muted">Choose an area to see coverage.</p>
        )}
      </Section>

      {a?.households && (
        <Section title="Placed household units">
          <div className="stats">
            <Stat label="Hear ≥ 1 powered" value={fmtPct(a.households.stats.pctHeard1)} tone={a.households.stats.pctHeard1 >= 95 ? "ok" : "bad"} />
            <Stat label="Hear ≥ 2 powered" value={fmtPct(a.households.stats.pctHeard2)} tone={a.households.stats.pctHeard2 >= 90 ? "ok" : "warn"} />
            <Stat
              label="SOS reaches responder"
              value={a.households.stats.hasResponders ? fmtPct(a.households.stats.pctSosWithNeighbours) : "–"}
              tone={!a.households.stats.hasResponders ? undefined : a.households.stats.pctSosWithNeighbours >= 95 ? "ok" : "bad"}
              sub={
                a.households.stats.hasResponders
                  ? `${fmtPct(a.households.stats.pctSosPoweredOnly)} via powered units alone`
                  : "place a responder"
              }
            />
            <Stat
              label="If every powered unit fails"
              value={a.households.stats.hasResponders ? fmtPct(a.households.stats.pctSosNoPowered) : "–"}
              tone={!a.households.stats.hasResponders ? undefined : a.households.stats.pctSosNoPowered >= 50 ? "ok" : "warn"}
              sub="SOS still reaches a responder"
            />
          </div>
          <table className="table compact">
            <tbody>
              <tr>
                <td>Household units each one hears directly (average)</td>
                <td className="num">{a.households.stats.meanNeighbours.toFixed(1)}</td>
              </tr>
              <tr>
                <td>Largest group still linked with every powered unit down</td>
                <td className="num">{fmtPct(a.households.stats.pctLargestGroup)}</td>
              </tr>
            </tbody>
          </table>
          <p className="hint">
            SOS counts relays through other household units too (every unit forwards SOS, §13.3), up to the hop limit of {plan.model.hopLimit}.
            {plan.householdPoints && plan.householdPoints.points.length < plan.householdPoints.total
              ? " These dots are a sample, sparser than the real units, so the through-neighbours figures are on the low side."
              : ""}{" "}
            Computed in {a.households.ms.toFixed(0)} ms.
          </p>
        </Section>
      )}

      {a && (
        <Section title="Network">
          <table className="table compact">
            <tbody>
              <tr>
                <td>Links between placed units</td>
                <td className="num">{fmtInt(a.backbone.edges.length)}</td>
              </tr>
              <tr>
                <td>Islands of powered units</td>
                <td className={`num ${a.poweredIslands > 1 ? "warnText" : ""}`}>{a.poweredIslands}</td>
              </tr>
              <tr>
                <td>Most relays from a powered unit to a responder</td>
                <td className="num">{a.maxRelays < 0 ? "–" : a.maxRelays}</td>
              </tr>
              <tr>
                <td>Powered units beyond the hop limit</td>
                <td className={`num ${a.poweredUnreachable ? "badText" : ""}`}>{a.poweredUnreachable}</td>
              </tr>
              <tr>
                <td>Airtime: SOS / text / heartbeat (SF{plan.model.sf}, with wake-up preamble)</td>
                <td className="num">
                  {(a.airtime.sosS * 1000).toFixed(0)} / {(a.airtime.textS * 1000).toFixed(0)} / {(a.airtime.heartbeatS * 1000).toFixed(0)} ms
                </td>
              </tr>
              <tr>
                <td>
                  Heartbeat channel time around the busiest powered unit (each sent once per hop)
                  {a.airtime.heartbeatEstimated && <span className="muted"> · estimated from the best-connected units</span>}
                </td>
                <td className="num">
                  {a.airtime.heartbeatChannelSPerHour.toFixed(0)} of ~{USABLE_CHANNEL_S_PER_HOUR} s/h
                </td>
              </tr>
              <tr>
                <td>Heartbeat relaying by the busiest unit if the race failed (worst case)</td>
                <td className="num">
                  {a.airtime.worstHeartbeatSPerHour.toFixed(0)} of {a.airtime.budgetSPerHour.toFixed(0)} s/h
                </td>
              </tr>
              <tr>
                <td>SOS sends per hour within one unit's duty budget</td>
                <td className="num">{fmtInt(a.airtime.budgetSPerHour / a.airtime.sosS)}</td>
              </tr>
              <tr>
                <td>Battery-only fallback: household units each hears ({fmtM(a.ranges.householdHousehold)} reach, {fmtInt(density)} units/km²)</td>
                <td className={`num ${nbrs < PERCOLATION_MEAN_DEGREE ? "warnText" : ""}`}>{nbrs.toFixed(1)}</td>
              </tr>
            </tbody>
          </table>
          <p className="hint">
            Fallback: with every powered unit down, household units still form one mesh when each hears about {PERCOLATION_MEAN_DEGREE} others. Computed in{" "}
            {a.ms.toFixed(0)} ms.
          </p>
        </Section>
      )}
    </div>
  );
}
