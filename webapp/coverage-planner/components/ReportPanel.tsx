"use client";
/** Report details, printing, saving and opening plans, and exports. */
import { useRef } from "react";
import { download, slug, toCsv, toGeoJson, toKml } from "@/lib/exporters";
import { toPlanFile, type PlanState, type ReportInfo } from "@/lib/planState";
import { Section } from "./ui";

interface Props {
  plan: PlanState;
  setReport: (r: Partial<ReportInfo>) => void;
  openPlan: (file: File) => void;
  newPlan: () => void;
  print: () => void;
}

export default function ReportPanel({ plan, setReport, openPlan, newPlan, print }: Props) {
  const fileRef = useRef<HTMLInputElement>(null);
  const name = plan.report.title || plan.area?.name || "FloodMesh plan";
  const base = `floodmesh-${slug(name)}`;
  const r = plan.report;
  const profiles = { powered: plan.profiles.powered, responder: plan.profiles.responder };
  return (
    <div>
      <Section title="Report">
        <div className="field">
          <label htmlFor="rt">Title</label>
          <input id="rt" value={r.title} maxLength={200} placeholder={`FloodMesh deployment plan: ${plan.area?.name ?? "area"}`} onChange={(e) => setReport({ title: e.target.value })} />
        </div>
        <div className="field">
          <label htmlFor="rf">Prepared for</label>
          <input id="rf" value={r.preparedFor} maxLength={200} placeholder="e.g. Greater Chennai Corporation, Zone 12" onChange={(e) => setReport({ preparedFor: e.target.value })} />
        </div>
        <div className="field">
          <label htmlFor="rb">Prepared by</label>
          <input id="rb" value={r.preparedBy} maxLength={200} onChange={(e) => setReport({ preparedBy: e.target.value })} />
        </div>
        <div className="field">
          <label htmlFor="rn">Notes</label>
          <textarea id="rn" rows={4} maxLength={4000} value={r.notes} onChange={(e) => setReport({ notes: e.target.value })} />
        </div>
        <div className="row">
          <button type="button" onClick={print}>
            Print or save as PDF
          </button>
        </div>
        <p className="hint">The printout has the map as it is on screen, the coverage results, the checks, the cost estimate and the assumptions. Choose landscape for a wider map.</p>
      </Section>

      <Section title="Plan file">
        <div className="row wrap">
          <button type="button" onClick={() => download(`${base}.json`, JSON.stringify(toPlanFile(plan), null, 1), "application/json")}>
            Save plan
          </button>
          <button type="button" className="secondary" onClick={() => fileRef.current?.click()}>
            Open plan
          </button>
          <button type="button" className="secondary" onClick={newPlan}>
            New plan
          </button>
          <input
            ref={fileRef}
            type="file"
            accept=".json,application/json"
            hidden
            onChange={(e) => {
              const f = e.target.files?.[0];
              if (f) openPlan(f);
              e.target.value = "";
            }}
          />
        </div>
        <p className="hint">The plan also stays in this browser between visits. Prices are not in the file; they always come from the admin's price list.</p>
      </Section>

      <Section title="Export for other tools">
        <div className="row wrap">
          <button
            type="button"
            className="secondary"
            onClick={() => download(`${base}.kml`, toKml(name, plan.area?.polygons ?? [], plan.units), "application/vnd.google-earth.kml+xml")}
          >
            KML (Google Earth)
          </button>
          <button
            type="button"
            className="secondary"
            onClick={() => download(`${base}.geojson`, toGeoJson(name, plan.area?.polygons ?? [], plan.units), "application/geo+json")}
          >
            GeoJSON (QGIS)
          </button>
          <button type="button" className="secondary" onClick={() => download(`${base}-units.csv`, toCsv(plan.units, profiles), "text/csv")}>
            Unit list (CSV)
          </button>
        </div>
      </Section>
    </div>
  );
}
