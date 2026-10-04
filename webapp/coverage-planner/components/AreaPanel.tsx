"use client";
/** Choose the planning area: a constituency, an OSM locality, a drawn shape or a GeoJSON file. */
import { useMemo, useRef, useState } from "react";
import { normName } from "@/lib/names";
import { searchPlaces, type PlaceResult } from "@/lib/osm";
import type { PolygonRings } from "@/lib/plan";
import type { AreaState } from "@/lib/planState";
import { circleRings, geometryToPolygons, type DrawShape } from "./mapController";
import { Check, NumberField, Section, fmtInt } from "./ui";

export interface AcFeature {
  type: "Feature";
  properties: { ac_no: number; name: string; district: string; pc: string };
  geometry: GeoJSON.Polygon | GeoJSON.MultiPolygon;
}
export interface AcCollection {
  type: "FeatureCollection";
  attribution?: string;
  features: AcFeature[];
}

/** Other spellings people search for (the dataset spells some names differently). */
const AC_ALIASES: Record<number, string[]> = {
  9: ["Madhavaram"],
  10: ["Thiruvottiyur"],
  11: ["RK Nagar", "Radhakrishnan Nagar"],
  15: ["TVK Nagar", "Thiru Vi Ka Nagar"],
  19: ["Chepauk", "Triplicane", "Thiruvallikeni"],
  22: ["Virugambakkam"],
  24: ["T Nagar", "T. Nagar", "Thyagaraya Nagar", "Theagaraya Nagar"],
  27: ["Sholinganallur"],
};

const SOURCE_LABELS: Record<string, string> = {
  constituency: "assembly constituency",
  locality: "OpenStreetMap boundary",
  circle: "circle around an OpenStreetMap point",
  drawn: "drawn on the map",
  file: "imported from a file",
};

interface Props {
  area: AreaState | null;
  areaKm2: number;
  acs: AcCollection | null;
  acsError: string | null;
  loadAcs: () => void;
  showAcOutlines: boolean;
  setShowAcOutlines: (v: boolean) => void;
  setArea: (a: AreaState | null) => void;
  drawing: DrawShape | null;
  startDraw: (s: DrawShape) => void;
  cancelDraw: () => void;
  editing: boolean;
  toggleEdit: () => void;
  notify: (msg: string) => void;
}

export default function AreaPanel(p: Props) {
  const [acQuery, setAcQuery] = useState("");
  const [placeQuery, setPlaceQuery] = useState("");
  const [nearChennai, setNearChennai] = useState(true);
  const [places, setPlaces] = useState<PlaceResult[] | null>(null);
  const [searching, setSearching] = useState(false);
  const [searchError, setSearchError] = useState<string | null>(null);
  const [radiusM, setRadiusM] = useState(1000);
  const fileRef = useRef<HTMLInputElement>(null);

  const acMatches = useMemo(() => {
    if (!p.acs) return [];
    const q = normName(acQuery);
    const list = p.acs.features.filter((f) => {
      if (!q) return f.properties.district === "Chennai";
      const keys = [f.properties.name, f.properties.district, String(f.properties.ac_no), ...(AC_ALIASES[f.properties.ac_no] ?? [])];
      return keys.some((k) => normName(k).includes(q)) || String(f.properties.ac_no) === acQuery.trim();
    });
    return list.slice(0, 40);
  }, [p.acs, acQuery]);

  function pickAc(f: AcFeature, add: boolean) {
    const polys = geometryToPolygons(f.geometry);
    const cur = p.area;
    if (add && cur && cur.source === "constituency" && cur.acNos) {
      if (cur.acNos.includes(f.properties.ac_no)) return;
      p.setArea({
        name: `${cur.name} + ${f.properties.name}`,
        source: "constituency",
        polygons: [...cur.polygons, ...polys],
        acNos: [...cur.acNos, f.properties.ac_no],
      });
    } else {
      p.setArea({
        name: `${f.properties.name} (AC ${f.properties.ac_no})`,
        source: "constituency",
        polygons: polys,
        acNos: [f.properties.ac_no],
      });
    }
  }

  async function search() {
    const q = placeQuery.trim();
    if (!q) return;
    setSearching(true);
    setSearchError(null);
    try {
      setPlaces(await searchPlaces(q, nearChennai));
    } catch (err) {
      setPlaces(null);
      setSearchError(err instanceof Error ? err.message : "Place search failed.");
    } finally {
      setSearching(false);
    }
  }

  function pickPlace(r: PlaceResult) {
    if (r.polygons.length) {
      p.setArea({ name: r.name, source: "locality", polygons: r.polygons });
    } else {
      p.setArea({ name: `${r.name} (${(radiusM / 1000).toFixed(1)} km circle)`, source: "circle", polygons: [circleRings(r.lat, r.lng, radiusM)] });
      p.notify(`OpenStreetMap has no boundary for ${r.name}; using a ${radiusM} m circle. Edit or redraw it to match the ward.`);
    }
  }

  async function importFile(file: File) {
    try {
      const j = JSON.parse(await file.text()) as GeoJSON.GeoJSON;
      const polys: PolygonRings[] =
        j.type === "FeatureCollection"
          ? j.features.flatMap((f) => geometryToPolygons(f.geometry))
          : j.type === "Feature"
            ? geometryToPolygons(j.geometry)
            : geometryToPolygons(j as GeoJSON.Geometry);
      if (!polys.length) throw new Error("No polygons in this file.");
      p.setArea({ name: file.name.replace(/\.(geo)?json$/i, ""), source: "file", polygons: polys });
    } catch (err) {
      p.notify(`Could not read ${file.name}: ${err instanceof Error ? err.message : "not GeoJSON"}`);
    }
  }

  return (
    <div>
      <div className="areaNow">
        {p.area ? (
          <>
            <div className="statLabel">Planning area</div>
            <div className="areaName">{p.area.name}</div>
            <div className="muted">
              {p.areaKm2.toFixed(2)} km² · {SOURCE_LABELS[p.area.source] ?? "imported"}
            </div>
            <div className="row">
              <button type="button" className="secondary" onClick={p.toggleEdit}>
                {p.editing ? "Finish editing shape" : "Edit shape"}
              </button>
              <button type="button" className="secondary" onClick={() => p.setArea(null)}>
                Clear area
              </button>
            </div>
          </>
        ) : (
          <p className="muted">No area yet. Pick a constituency, search a locality, or draw one on the map.</p>
        )}
      </div>

      <Section title="Assembly constituency">
        {!p.acs ? (
          <div className="row">
            <button type="button" onClick={p.loadAcs}>Load Tamil Nadu constituencies</button>
            {p.acsError && <span className="badText">{p.acsError}</span>}
          </div>
        ) : (
          <>
            <input
              type="search"
              placeholder="e.g. Alandur, Maduravoyal, Velachery, 28"
              value={acQuery}
              onChange={(e) => setAcQuery(e.target.value)}
              aria-label="Search constituencies"
            />
            <ul className="pickList">
              {acMatches.map((f) => (
                <li key={f.properties.ac_no}>
                  <button type="button" className="linkish" onClick={() => pickAc(f, false)}>
                    <strong>{f.properties.name}</strong> <span className="muted">AC {f.properties.ac_no} · {f.properties.district}</span>
                  </button>
                  {p.area?.source === "constituency" && (
                    <button type="button" className="mini" title="Add to the current area" onClick={() => pickAc(f, true)}>
                      + add
                    </button>
                  )}
                </li>
              ))}
              {acMatches.length === 0 && <li className="muted">No match.</li>}
            </ul>
            <Check label="Show constituency outlines on the map (click one to pick it)" checked={p.showAcOutlines} onChange={p.setShowAcOutlines} />
            <p className="hint">
              {fmtInt(p.acs.features.length)} constituencies, 2008 delimitation. Districts are as at delimitation (Alandur is listed under
              Kancheepuram). Boundaries: DataMeet India (CC BY 2.5 IN), scraped from ECI maps; approximate.
            </p>
          </>
        )}
      </Section>

      <Section title="Locality (OpenStreetMap)">
        <form
          className="row"
          onSubmit={(e) => {
            e.preventDefault();
            void search();
          }}
        >
          <input
            type="search"
            placeholder="e.g. Anna Nagar, Nandambakkam, Mudichur"
            value={placeQuery}
            onChange={(e) => setPlaceQuery(e.target.value)}
            aria-label="Search a locality"
            style={{ flex: 1 }}
          />
          <button type="submit" disabled={searching || !placeQuery.trim()}>
            {searching ? "Searching…" : "Search"}
          </button>
        </form>
        <Check label="Prefer results around Chennai" checked={nearChennai} onChange={setNearChennai} />
        {searchError && <p className="badText">{searchError}</p>}
        {places && (
          <ul className="pickList">
            {places.map((r) => (
              <li key={r.id}>
                <button type="button" className="linkish" onClick={() => pickPlace(r)} title={r.display}>
                  <strong>{r.name}</strong>{" "}
                  <span className="muted">
                    {r.kind} · {r.polygons.length ? "boundary" : "point only"} · {r.display.split(",").slice(1, 3).join(",")}
                  </span>
                </button>
              </li>
            ))}
            {places.length === 0 && <li className="muted">Nothing found. Try another spelling or add “, Chennai”.</li>}
          </ul>
        )}
        <NumberField label="Circle radius when OSM has no boundary" value={radiusM} onChange={setRadiusM} min={100} max={20000} step={100} unit="m" />
        <p className="hint">Search: Nominatim (© OpenStreetMap contributors). Many Chennai neighbourhoods are mapped as points only; check the outline against the ward map.</p>
      </Section>

      <Section title="Draw or import">
        <div className="row wrap">
          {(["Polygon", "Rectangle", "Circle"] as DrawShape[]).map((s) => (
            <button key={s} type="button" className={p.drawing === s ? "" : "secondary"} onClick={() => (p.drawing === s ? p.cancelDraw() : p.startDraw(s))}>
              {p.drawing === s ? `Drawing ${s.toLowerCase()}… (cancel)` : `Draw ${s.toLowerCase()}`}
            </button>
          ))}
          <button type="button" className="secondary" onClick={() => fileRef.current?.click()}>
            Import GeoJSON
          </button>
          <input
            ref={fileRef}
            type="file"
            accept=".geojson,.json,application/geo+json,application/json"
            hidden
            onChange={(e) => {
              const f = e.target.files?.[0];
              if (f) void importFile(f);
              e.target.value = "";
            }}
          />
        </div>
        <p className="hint">Polygon: click each corner, click the first point to finish. A drawn shape replaces the current area.</p>
      </Section>
    </div>
  );
}
