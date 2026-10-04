"use client";
/** Place powered and responder units, auto-place, and set how many household units the area needs. */
import { useState } from "react";
import type { PlacedUnit } from "@/lib/plan";
import { MAX_PLACED } from "@/lib/households";
import { DENSITY_PRESETS, demandPending, households, householdUnits, type Demand, type PlanState } from "@/lib/planState";
import { UNIT_LABELS } from "@/lib/radio";
import type { Analysis } from "./analysis";
import type { MapMode } from "./mapController";
import { Check, NumberField, Section, fmtInt, fmtM } from "./ui";

interface Props {
  plan: PlanState;
  analysis: Analysis | null;
  mode: MapMode;
  setMode: (m: MapMode) => void;
  selectedId: string | null;
  setSelectedId: (id: string | null) => void;
  updateUnit: (id: string, patch: Partial<PlacedUnit>) => void;
  removeUnit: (id: string) => void;
  clearUnits: (kind: PlacedUnit["kind"] | "all") => void;
  autoPlace: (redundancy: 1 | 2, keep: boolean, factor: number) => void;
  addResponderAtCentre: () => void;
  setDemand: (d: Partial<Demand>) => void;
  setPlanField: <K extends keyof PlanState>(k: K, v: PlanState[K]) => void;
  countBuildings: () => void;
  countingBuildings: boolean;
  /** A building count or a placement is talking to OSM: one at a time. */
  osmBusy: boolean;
  placeHouseholds: (source: "osm" | "random") => void;
  placingHouseholds: boolean;
  clearHouseholds: () => void;
  panToUnit: (u: PlacedUnit) => void;
}

const MODES: { id: MapMode; label: string; help: string }[] = [
  { id: "pan", label: "Move / select", help: "Drag units to their real terraces; click one to edit it." },
  { id: "add-powered", label: "+ Powered unit", help: "Click the map where a terrace unit on solar or UPS power will go." },
  { id: "add-responder", label: "+ Responder", help: "Click where a responder team or control room will be." },
  { id: "probe", label: "Probe signal", help: "Click any spot to see what a household unit there would hear." },
];

export default function UnitsPanel(p: Props) {
  const { plan, analysis } = p;
  const [keep, setKeep] = useState(false);
  const [factor, setFactor] = useState(0.9);
  const sel = plan.units.find((u) => u.id === p.selectedId) ?? null;
  const powered = plan.units.filter((u) => u.kind === "powered").length;
  const responders = plan.units.filter((u) => u.kind === "responder").length;
  const km2 = analysis?.areaKm2 ?? 0;
  const hh = households(plan.demand, km2);
  const hhu = householdUnits(plan.demand, km2);
  const d = plan.demand;

  return (
    <div>
      <Section title="Map tool">
        <div className="segmented" role="group" aria-label="Map tool">
          {MODES.map((m) => (
            <button key={m.id} type="button" className={p.mode === m.id ? "on" : ""} onClick={() => p.setMode(m.id)} aria-pressed={p.mode === m.id}>
              {m.label}
            </button>
          ))}
        </div>
        <p className="hint">{MODES.find((m) => m.id === p.mode)?.help}</p>
      </Section>

      <Section title="Auto-place powered units">
        <div className="segmented" role="group" aria-label="Redundancy">
          {([1, 2] as const).map((r) => (
            <button key={r} type="button" className={plan.redundancy === r ? "on" : ""} onClick={() => p.setPlanField("redundancy", r)}>
              {r === 1 ? "Every house hears 1" : "Every house hears 2 (§4.2)"}
            </button>
          ))}
        </div>
        <NumberField
          label="Spacing safety factor"
          value={factor}
          onChange={setFactor}
          min={0.4}
          max={1.2}
          step={0.05}
          hint="Below 1 packs units closer than the model needs, as a margin for buildings the model can't see."
        />
        <Check label="Keep the powered units already placed" checked={keep} onChange={setKeep} />
        <div className="row wrap">
          <button type="button" disabled={!plan.area} onClick={() => p.autoPlace(plan.redundancy, keep, factor)}>
            Auto-place
          </button>
          <button type="button" className="secondary" disabled={!plan.area} onClick={p.addResponderAtCentre}>
            Responder at area centre
          </button>
        </div>
        <p className="hint">
          Household ↔ powered range now: <strong>{analysis ? fmtM(analysis.ranges.householdPowered) : "–"}</strong>. Auto-place uses a triangular
          lattice ({plan.redundancy === 2 ? "spacing = range" : "spacing = range × √3"}) and fills the gaps at the edges. Then drag each unit onto a
          real terrace with power.
        </p>
      </Section>

      <Section title="Household units">
        <div className="segmented" role="group" aria-label="How to count households">
          {(
            [
              ["density", "By density"],
              ["osm", "OSM buildings"],
              ["manual", "Enter number"],
            ] as const
          ).map(([id, label]) => (
            <button key={id} type="button" className={d.mode === id ? "on" : ""} onClick={() => p.setDemand({ mode: id })}>
              {label}
            </button>
          ))}
        </div>
        {d.mode === "density" && (
          <>
            <div className="field">
              <label htmlFor="dens">Preset</label>
              <select
                id="dens"
                value={DENSITY_PRESETS.some((x) => x.value === d.householdsPerKm2) ? d.householdsPerKm2 : ""}
                onChange={(e) => e.target.value && p.setDemand({ householdsPerKm2: Number(e.target.value) })}
              >
                <option value="">Custom</option>
                {DENSITY_PRESETS.map((x) => (
                  <option key={x.value} value={x.value}>
                    {x.label}
                  </option>
                ))}
              </select>
            </div>
            <NumberField label="Households per km²" value={d.householdsPerKm2} onChange={(v) => p.setDemand({ householdsPerKm2: v })} min={0} max={100000} step={100} />
          </>
        )}
        {d.mode === "osm" && (
          <>
            <div className="row">
              <button type="button" onClick={p.countBuildings} disabled={!plan.area || p.osmBusy}>
                {p.countingBuildings ? "Counting…" : "Count buildings in OSM"}
              </button>
              <span>{d.osmBuildings === null ? "not counted yet" : `${fmtInt(d.osmBuildings)} buildings`}</span>
            </div>
            <NumberField
              label="Households per building"
              value={d.householdsPerBuilding}
              onChange={(v) => p.setDemand({ householdsPerBuilding: v })}
              min={0}
              max={1000}
              step={0.5}
              hint="OSM misses many buildings in Indian cities, and an apartment block is one building; check against ward census data."
            />
          </>
        )}
        {d.mode === "manual" && (
          <NumberField label="Households in the area" value={d.manualHouseholds} onChange={(v) => p.setDemand({ manualHouseholds: v })} min={0} step={100} />
        )}
        <NumberField
          label="Households that get a unit"
          value={d.adoptionPct}
          onChange={(v) => p.setDemand({ adoptionPct: v })}
          min={0}
          max={100}
          step={5}
          unit="%"
          hint="For example only flood-prone streets, ground-floor homes, or the elderly."
        />
        <div className="kv">
          <span>Households</span>
          <strong>{demandPending(d) ? "count buildings first" : fmtInt(hh)}</strong>
          <span>Household units</span>
          <strong>{demandPending(d) ? "–" : fmtInt(hhu)}</strong>
        </div>
      </Section>

      <Section title="Place household units on the map">
        <div className="row wrap">
          <button type="button" disabled={!plan.area || p.osmBusy} onClick={() => p.placeHouseholds("osm")}>
            {p.placingHouseholds ? "Placing…" : "On OSM buildings"}
          </button>
          <button
            type="button"
            className="secondary"
            disabled={!plan.area || p.osmBusy || demandPending(d)}
            title={demandPending(d) ? "Count the OSM buildings first" : undefined}
            onClick={() => p.placeHouseholds("random")}
          >
            Randomly in the area
          </button>
          {plan.householdPoints && (
            <button type="button" className="secondary" onClick={p.clearHouseholds}>
              Remove
            </button>
          )}
        </div>
        {plan.householdPoints ? (
          <p className="hint">
            <strong>{fmtInt(plan.householdPoints.points.length)}</strong> placed{" "}
            {plan.householdPoints.source !== "osm"
              ? "evenly over the area"
              : plan.householdPoints.onBuildings === undefined || plan.householdPoints.onBuildings >= plan.householdPoints.points.length
                ? "on OpenStreetMap buildings"
                : `: ${fmtInt(plan.householdPoints.onBuildings)} on OpenStreetMap buildings, the rest spread evenly`}
            {plan.householdPoints.points.length < plan.householdPoints.total
              ? `, a sample of the plan's ${fmtInt(plan.householdPoints.total)}`
              : ""}
            .
            {!demandPending(d) && hhu !== plan.householdPoints.total && (
              <span className="warnText"> The plan now has {fmtInt(hhu)} household units: place them again to match.</span>
            )}
          </p>
        ) : (
          <p className="hint">
            One dot per household unit (up to {fmtInt(MAX_PLACED)}; larger plans get an even sample, and the cost still uses the full count).
            Each dot shows what that unit hears, and whether its SOS gets out through neighbours&apos; units too. “On OSM buildings” also counts
            the buildings when households are counted from OSM.
          </p>
        )}
      </Section>

      <Section title="Responders">
        <NumberField
          label="Extra responder handsets (not placed on the map)"
          value={plan.extraResponders}
          onChange={(v) => p.setPlanField("extraResponders", Math.round(v))}
          min={0}
          max={100000}
        />
      </Section>

      {sel && (
        <Section title={`Selected: ${sel.name}`} aside={<button type="button" className="mini" onClick={() => p.setSelectedId(null)}>close</button>}>
          <div className="field">
            <label htmlFor="uname">Name</label>
            <input id="uname" value={sel.name} maxLength={40} onChange={(e) => p.updateUnit(sel.id, { name: e.target.value })} />
          </div>
          <NumberField
            label="Antenna height above ground"
            value={sel.heightM ?? plan.profiles[sel.kind].heightM}
            onChange={(v) => p.updateUnit(sel.id, { heightM: v })}
            min={0.5}
            max={200}
            step={0.5}
            unit="m"
          />
          <NumberField
            label="Antenna gain"
            value={sel.antennaDbi ?? plan.profiles[sel.kind].antennaDbi}
            onChange={(v) => p.updateUnit(sel.id, { antennaDbi: v })}
            min={-10}
            max={20}
            step={0.1}
            unit="dBi"
          />
          <p className="hint">
            {UNIT_LABELS[sel.kind]} at {sel.lat.toFixed(5)}, {sel.lng.toFixed(5)}. Reach to household units: {fmtM(analysis?.labels.get(sel.id)?.rangeM ?? 0)}.
          </p>
          <div className="row">
            <button type="button" className="danger" onClick={() => p.removeUnit(sel.id)}>
              Delete unit
            </button>
          </div>
        </Section>
      )}

      <Section
        title={`Placed units (${powered} powered, ${responders} responder)`}
        aside={
          plan.units.length > 0 ? (
            <button type="button" className="mini" onClick={() => p.clearUnits("all")}>
              clear all
            </button>
          ) : undefined
        }
      >
        {plan.units.length === 0 ? (
          <p className="muted">None yet.</p>
        ) : (
          <div className="tableWrap">
            <table className="table compact">
              <thead>
                <tr>
                  <th>Name</th>
                  <th>Type</th>
                  <th>Height</th>
                  <th>Relays to responder</th>
                </tr>
              </thead>
              <tbody>
                {plan.units.map((u, i) => {
                  const r = analysis?.backbone.relaysToResponder[i] ?? -1;
                  return (
                    <tr
                      key={u.id}
                      className={u.id === p.selectedId ? "selected" : ""}
                      onClick={() => {
                        p.setSelectedId(u.id);
                        p.panToUnit(u);
                      }}
                    >
                      <td>{u.name}</td>
                      <td>{u.kind === "powered" ? "Powered" : "Responder"}</td>
                      <td>{u.heightM ?? plan.profiles[u.kind].heightM} m</td>
                      <td className={u.kind === "powered" && (r < 0 || r > plan.model.hopLimit) ? "badText" : ""}>
                        {u.kind === "responder" ? "–" : r < 0 ? "none" : r}
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </Section>
    </div>
  );
}
