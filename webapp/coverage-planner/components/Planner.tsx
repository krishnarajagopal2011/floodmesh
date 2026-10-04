"use client";
/**
 * The coverage planner: OpenStreetMap on the left, details and tools on the
 * right. Holds the plan state; the map (mapController.ts) only draws it and
 * reports clicks, drags and drawn shapes back.
 */
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import "leaflet/dist/leaflet.css";
import "@geoman-io/leaflet-geoman-free/dist/leaflet-geoman.css";
import { formatInr, type PublicCosts } from "@/lib/costs";
import { MAX_PLACED, placeOnBuildings, placeRandom, seedOf } from "@/lib/households";
import { buildingCentres, countBuildings } from "@/lib/osm";
import { autoPlace, cellXY, type PlacedUnit } from "@/lib/plan";
import {
  defaultPlan,
  demandPending,
  fromPlanFile,
  householdUnits,
  sanitizePolygons,
  toPlanFile,
  type AreaState,
  type Demand,
  type PlanState,
  type ReportInfo,
} from "@/lib/planState";
import { DEFAULT_MODEL, DEFAULT_PROFILES, ENVIRONMENTS, type RadioModel, type UnitKind, type UnitProfile } from "@/lib/radio";
import { PROBE_ROWS, analyse, householdColours, probe, probeHtml, renderCoverage, type Analysis, type CoverageView } from "./analysis";
import AreaPanel, { type AcCollection } from "./AreaPanel";
import CostPanel, { planEstimate } from "./CostPanel";
import CoveragePanel, { Legend } from "./CoveragePanel";
import type { DrawShape, MapCallbacks, MapController, MapMode } from "./mapController";
import PrintReport from "./PrintReport";
import RadioPanel from "./RadioPanel";
import ReportPanel from "./ReportPanel";
import { fmtInt, fmtPct } from "./ui";
import UnitsPanel from "./UnitsPanel";

type Tab = "area" | "units" | "radio" | "coverage" | "cost" | "report";
const TABS: { id: Tab; label: string }[] = [
  { id: "area", label: "Area" },
  { id: "units", label: "Units" },
  { id: "coverage", label: "Coverage" },
  { id: "cost", label: "Cost" },
  { id: "radio", label: "Radio" },
  { id: "report", label: "Report" },
];
const STORAGE_KEY = "floodmesh-planner-plan";
const QUICK_MODES: { id: MapMode; label: string; title: string }[] = [
  { id: "pan", label: "Move", title: "Move the map and drag units" },
  { id: "add-powered", label: "+ Powered", title: "Click to place a powered terrace unit" },
  { id: "add-responder", label: "+ Responder", title: "Click to place a responder" },
  { id: "probe", label: "Probe", title: "Click to see what a household unit there would hear" },
];

let idCounter = 0;
function newId(): string {
  idCounter++;
  return `u${Date.now().toString(36)}${idCounter.toString(36)}${Math.floor(Math.random() * 1e6).toString(36)}`;
}

function nextName(units: PlacedUnit[], kind: PlacedUnit["kind"]): string {
  const prefix = kind === "powered" ? "P" : "R";
  let max = 0;
  for (const u of units) {
    const m = new RegExp(`^${prefix}(\\d+)$`).exec(u.name);
    if (m) max = Math.max(max, Number(m[1]));
  }
  return `${prefix}${max + 1}`;
}

export default function Planner() {
  const [plan, setPlan] = useState<PlanState>(defaultPlan);
  const [loaded, setLoaded] = useState(false);
  const [tab, setTab] = useState<Tab>("area");
  const [mode, setMode] = useState<MapMode>("pan");
  const [view, setView] = useState<CoverageView>("redundancy");
  const [opacity, setOpacity] = useState(0.5);
  const [showLinks, setShowLinks] = useState(true);
  const [showRanges, setShowRanges] = useState(false);
  const [showHouseholds, setShowHouseholds] = useState(true);
  const [placing, setPlacing] = useState(false);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [costs, setCosts] = useState<PublicCosts | null>(null);
  const [costsError, setCostsError] = useState<string | null>(null);
  const [acs, setAcs] = useState<AcCollection | null>(null);
  const [acsError, setAcsError] = useState<string | null>(null);
  const [showAcOutlines, setShowAcOutlines] = useState(false);
  const [drawing, setDrawing] = useState<DrawShape | null>(null);
  const [editing, setEditing] = useState(false);
  const [counting, setCounting] = useState(false);
  const [toast, setToast] = useState<string | null>(null);
  const [analysis, setAnalysis] = useState<Analysis | null>(null);
  const [mapReady, setMapReady] = useState(false);
  const [printing, setPrinting] = useState(false);

  const mapEl = useRef<HTMLDivElement>(null);
  const ctl = useRef<MapController | null>(null);
  const fitNext = useRef(false);
  const toastTimer = useRef<ReturnType<typeof setTimeout> | null>(null);

  const notify = useCallback((msg: string) => {
    setToast(msg);
    if (toastTimer.current) clearTimeout(toastTimer.current);
    toastTimer.current = setTimeout(() => setToast(null), 6000);
  }, []);

  // --------------------------------------------------------- persistence

  useEffect(() => {
    try {
      const raw = localStorage.getItem(STORAGE_KEY);
      if (raw) {
        setPlan(fromPlanFile(JSON.parse(raw)));
        fitNext.current = true;
      }
    } catch {
      // A damaged or old saved plan: start fresh.
    }
    setLoaded(true);
  }, []);

  useEffect(() => {
    if (!loaded) return;
    const t = setTimeout(() => {
      try {
        localStorage.setItem(STORAGE_KEY, JSON.stringify(toPlanFile(plan)));
      } catch {
        // Storage full or blocked: the plan file still works.
      }
    }, 400);
    return () => clearTimeout(t);
  }, [plan, loaded]);

  useEffect(() => {
    fetch("/api/costs", { cache: "no-store" })
      .then((r) => r.json())
      .then((j: { ok: boolean; costs?: PublicCosts; error?: string }) => {
        if (j.ok && j.costs) setCosts(j.costs);
        else setCostsError(j.error ?? "Prices are unavailable.");
      })
      .catch(() => setCostsError("Prices are unavailable (no connection to the server)."));
  }, []);

  const loadAcs = useCallback(() => {
    setAcsError(null);
    fetch("/data/tn-assembly-constituencies.geojson")
      .then((r) => {
        if (!r.ok) throw new Error(String(r.status));
        return r.json() as Promise<AcCollection>;
      })
      .then(setAcs)
      .catch(() => setAcsError("Could not load the constituency boundaries."));
  }, []);

  useEffect(() => {
    if (tab === "area" && !acs && !acsError) loadAcs();
  }, [tab, acs, acsError, loadAcs]);

  // ------------------------------------------------------------ analysis

  // The analysis depends on where units are and how they radiate, not on
  // their names: renaming a unit must not re-run it on every keystroke.
  const planRef = useRef(plan);
  planRef.current = plan;
  const radioUnitsKey = useMemo(
    () => plan.units.map((u) => `${u.id}|${u.kind}|${u.lat}|${u.lng}|${u.heightM ?? ""}|${u.antennaDbi ?? ""}`).join(";"),
    [plan.units],
  );
  useEffect(() => {
    const t = setTimeout(() => setAnalysis(analyse(planRef.current)), 60);
    return () => clearTimeout(t);
  }, [plan.area, radioUnitsKey, plan.model, plan.profiles, plan.householdPoints]);

  const image = useMemo(() => (analysis ? renderCoverage(analysis, view, plan.model.hopLimit) : null), [analysis, view, plan.model.hopLimit]);

  // ------------------------------------------------------- plan updates

  // A building count in flight belongs to the area it started on.
  const countReq = useRef<AbortController | null>(null);
  const cancelCount = useCallback(() => {
    countReq.current?.abort();
    countReq.current = null;
    setCounting(false);
    setPlacing(false);
  }, []);

  /**
   * A building count and placed household units only fit the area they were
   * made for: clear them, and say so if they were in use.
   */
  const dropBuildingCount = useCallback(
    (p: PlanState): PlanState => {
      const said: string[] = [];
      if (p.demand.mode === "osm" && p.demand.osmBuildings !== null) said.push("count the OSM buildings again");
      if (p.householdPoints) said.push("place the household units again");
      if (said.length) setTimeout(() => notify(`The area changed: ${said.join(" and ")} (Units tab).`), 0);
      return { ...p, demand: { ...p.demand, osmBuildings: null }, householdPoints: null };
    },
    [notify],
  );

  const setArea = useCallback(
    (area: AreaState | null) => {
      let next = area;
      if (area) {
        const polygons = sanitizePolygons(area.polygons);
        if (!polygons.length) {
          notify("That shape has no usable outline (coordinates missing or off the map).");
          return;
        }
        next = { ...area, polygons };
      }
      cancelCount();
      fitNext.current = true;
      setEditing(false);
      setPlan((p) => dropBuildingCount({ ...p, area: next }));
    },
    [cancelCount, dropBuildingCount, notify],
  );

  const updateUnit = useCallback((id: string, patch: Partial<PlacedUnit>) => {
    setPlan((p) => ({ ...p, units: p.units.map((u) => (u.id === id ? { ...u, ...patch } : u)) }));
  }, []);

  const removeUnit = useCallback((id: string) => {
    setPlan((p) => ({ ...p, units: p.units.filter((u) => u.id !== id) }));
    setSelectedId((s) => (s === id ? null : s));
  }, []);

  const clearUnits = useCallback((kind: PlacedUnit["kind"] | "all") => {
    if (!window.confirm(kind === "all" ? "Remove every placed unit?" : `Remove every ${kind} unit?`)) return;
    setPlan((p) => ({ ...p, units: kind === "all" ? [] : p.units.filter((u) => u.kind !== kind) }));
    setSelectedId(null);
  }, []);

  const setDemand = useCallback((d: Partial<Demand>) => setPlan((p) => ({ ...p, demand: { ...p.demand, ...d } })), []);
  const setModel = useCallback(
    (m: Partial<RadioModel>) =>
      setPlan((p) => ({
        ...p,
        model: { ...p.model, ...m },
        envId: m.clutterDb !== undefined ? (ENVIRONMENTS.find((e) => e.clutterDb === m.clutterDb)?.id ?? "custom") : p.envId,
      })),
    [],
  );
  const setEnv = useCallback((id: string) => {
    const env = ENVIRONMENTS.find((e) => e.id === id);
    setPlan((p) => ({ ...p, envId: id, model: env ? { ...p.model, clutterDb: env.clutterDb } : p.model }));
  }, []);
  const setProfile = useCallback(
    (k: UnitKind, pr: Partial<UnitProfile>) => setPlan((p) => ({ ...p, profiles: { ...p.profiles, [k]: { ...p.profiles[k], ...pr } } })),
    [],
  );
  const setReport = useCallback((r: Partial<ReportInfo>) => setPlan((p) => ({ ...p, report: { ...p.report, ...r } })), []);
  const setPlanField = useCallback(<K extends keyof PlanState>(k: K, v: PlanState[K]) => setPlan((p) => ({ ...p, [k]: v })), []);

  const runAutoPlace = useCallback(
    (redundancy: 1 | 2, keep: boolean, factor: number) => {
      const a = analyse(plan);
      if (!a.area || !a.grid) return;
      const kept = keep ? plan.units : plan.units.filter((u) => u.kind !== "powered");
      const pts = autoPlace(a.area, a.grid, kept, plan.profiles, plan.model, a.proj, redundancy, factor);
      const units = [...kept];
      for (const [lng, lat] of pts) units.push({ id: newId(), kind: "powered", lat, lng, name: nextName(units, "powered") });
      setPlan((p) => ({ ...p, units, redundancy }));
      setSelectedId(null);
      notify(
        pts.length >= 3000
          ? "Stopped at 3,000 new units: the area is very large for the current range. Check the radio settings."
          : `Placed ${pts.length} powered unit${pts.length === 1 ? "" : "s"}. Drag each onto a real terrace with power.`,
      );
    },
    [plan, notify],
  );

  const addResponderAtCentre = useCallback(() => {
    const a = analysis;
    if (!a?.area || !a.grid || a.grid.inside.length === 0) return;
    const cx = (a.area.minX + a.area.maxX) / 2;
    const cy = (a.area.minY + a.area.maxY) / 2;
    let best = 0;
    let bestD = Infinity;
    for (let k = 0; k < a.grid.inside.length; k++) {
      const [x, y] = cellXY(a.grid, a.grid.inside[k]);
      const d = (x - cx) ** 2 + (y - cy) ** 2;
      if (d < bestD) {
        bestD = d;
        best = k;
      }
    }
    const [x, y] = cellXY(a.grid, a.grid.inside[best]);
    const [lng, lat] = a.proj.toLngLat(x, y);
    setPlan((p) => ({ ...p, units: [...p.units, { id: newId(), kind: "responder", lat, lng, name: nextName(p.units, "responder") }] }));
  }, [analysis]);

  const doCountBuildings = useCallback(async () => {
    const area = plan.area;
    if (!area) return;
    countReq.current?.abort();
    const ctrl = new AbortController();
    countReq.current = ctrl;
    setCounting(true);
    try {
      const n = await countBuildings(area.polygons, ctrl.signal);
      // Only for the area it was made for: the area may have changed meanwhile.
      if (ctrl.signal.aborted) return;
      setPlan((p) => (p.area === area ? { ...p, demand: { ...p.demand, osmBuildings: n } } : p));
      notify(`${fmtInt(n)} buildings mapped in OpenStreetMap inside ${area.name}.`);
    } catch (err) {
      if (ctrl.signal.aborted) return;
      notify(err instanceof Error ? err.message : "Building count failed.");
    } finally {
      if (countReq.current === ctrl) {
        countReq.current = null;
        setCounting(false);
      }
    }
  }, [plan.area, notify]);

  /**
   * Place the plan's household units as points: on OSM building positions or
   * spread evenly. Large plans get a sample of MAX_PLACED; the cost still uses
   * the full count. On buildings, the fetched positions also give the
   * building count when households are counted from OSM.
   */
  const placeHouseholds = useCallback(
    async (source: "osm" | "random") => {
      const area = plan.area;
      const a = analysis;
      if (!area || !a?.area) return;
      countReq.current?.abort();
      const ctrl = new AbortController();
      countReq.current = ctrl;
      setPlacing(true);
      try {
        let demand = plan.demand;
        let buildings: [number, number][] = [];
        if (source === "osm") {
          buildings = await buildingCentres(area.polygons, ctrl.signal);
          if (ctrl.signal.aborted) return;
          if (!buildings.length) {
            notify("OpenStreetMap has no buildings mapped in this area. Use “Randomly in the area” instead.");
            return;
          }
          if (demand.mode === "osm") demand = { ...demand, osmBuildings: buildings.length };
        }
        const total = householdUnits(demand, a.areaKm2);
        if (total <= 0) {
          notify("The plan has no household units to place: set the households and the share that get a unit first.");
          return;
        }
        const n = Math.min(total, MAX_PLACED);
        const seed = seedOf(`${area.name}|${n}|${source}`);
        const points =
          source === "osm" ? placeOnBuildings(buildings, a.area, a.proj, n, seed) : placeRandom(a.area, a.proj, n, seed);
        setPlan((p) => (p.area === area ? { ...p, demand, householdPoints: { source, points, total } } : p));
        setShowHouseholds(true);
        notify(
          `Placed ${fmtInt(points.length)} household units${source === "osm" ? ` on ${fmtInt(buildings.length)} OSM buildings` : ""}` +
            (points.length < total ? ` (a sample of ${fmtInt(total)}; the cost uses all of them).` : "."),
        );
      } catch (err) {
        if (!ctrl.signal.aborted) notify(err instanceof Error ? err.message : "Placing household units failed.");
      } finally {
        if (countReq.current === ctrl) {
          countReq.current = null;
          setPlacing(false);
        }
      }
    },
    [plan.area, plan.demand, analysis, notify],
  );

  const clearHouseholds = useCallback(() => setPlan((p) => ({ ...p, householdPoints: null })), []);

  const openPlan = useCallback(
    async (file: File) => {
      try {
        const next = fromPlanFile(JSON.parse(await file.text()));
        cancelCount();
        fitNext.current = true;
        setPlan(next);
        setSelectedId(null);
        notify(`Opened ${file.name}.`);
      } catch (err) {
        notify(`Could not open ${file.name}: ${err instanceof Error ? err.message : "not a plan file"}`);
      }
    },
    [notify, cancelCount],
  );

  const newPlan = useCallback(() => {
    if (!window.confirm("Start a new plan? The current one is lost unless you saved it.")) return;
    cancelCount();
    setPlan(defaultPlan());
    setSelectedId(null);
  }, [cancelCount]);

  const resetRadio = useCallback(() => {
    setPlan((p) => ({ ...p, model: { ...DEFAULT_MODEL }, envId: "urban", profiles: structuredClone(DEFAULT_PROFILES) as Record<UnitKind, UnitProfile> }));
  }, []);

  // ----------------------------------------------------------------- map

  const handlers = useRef<MapCallbacks>(null as unknown as MapCallbacks);
  handlers.current = {
    onAddUnit: (kind, lat, lng) => {
      const id = newId();
      setPlan((p) => ({ ...p, units: [...p.units, { id, kind, lat, lng, name: nextName(p.units, kind) }] }));
      setSelectedId(id);
    },
    onMoveUnit: (id, lat, lng) => updateUnit(id, { lat, lng }),
    onSelectUnit: (id) => {
      setSelectedId(id);
      setTab("units");
    },
    onProbe: (lat, lng) => {
      if (!analysis) return;
      const rows = probe(plan, analysis, lat, lng);
      ctl.current?.showProbe(
        lat,
        lng,
        probeHtml(rows, plan),
        rows
          .slice(0, PROBE_ROWS)
          .filter((r) => r.marginDb >= 0)
          .map((r) => ({ a: [lat, lng], b: [r.unit.lat, r.unit.lng], marginDb: r.marginDb })),
      );
    },
    onDrawn: (polygons) => {
      setDrawing(null);
      setArea({ name: "Drawn area", source: "drawn", polygons });
    },
    onDrawEnd: () => setDrawing(null),
    onAreaEdited: (polygons) => {
      cancelCount();
      setPlan((p) =>
        p.area
          ? dropBuildingCount({
              ...p,
              area: { ...p.area, polygons, acNos: undefined, source: p.area.source === "constituency" ? "drawn" : p.area.source },
            })
          : p,
      );
    },
    onPickConstituency: (acNo) => {
      const f = acs?.features.find((x) => x.properties.ac_no === acNo);
      if (!f) return;
      const g = f.geometry;
      setArea({
        name: `${f.properties.name} (AC ${f.properties.ac_no})`,
        source: "constituency",
        polygons: g.type === "Polygon" ? [g.coordinates as [number, number][][]] : (g.coordinates as [number, number][][][]),
        acNos: [acNo],
      });
    },
  };

  useEffect(() => {
    let cancelled = false;
    let c: MapController | null = null;
    void import("./mapController").then(async ({ createMap }) => {
      if (cancelled || !mapEl.current) return;
      const h = handlers;
      c = await createMap(mapEl.current, {
        onAddUnit: (...a) => h.current.onAddUnit(...a),
        onMoveUnit: (...a) => h.current.onMoveUnit(...a),
        onSelectUnit: (...a) => h.current.onSelectUnit(...a),
        onProbe: (...a) => h.current.onProbe(...a),
        onDrawn: (...a) => h.current.onDrawn(...a),
        onDrawEnd: () => h.current.onDrawEnd(),
        onAreaEdited: (...a) => h.current.onAreaEdited(...a),
        onPickConstituency: (...a) => h.current.onPickConstituency(...a),
      });
      if (cancelled) {
        c.destroy();
        return;
      }
      ctl.current = c;
      setMapReady(true);
    });
    return () => {
      cancelled = true;
      c?.destroy();
      ctl.current = null;
      setMapReady(false);
    };
  }, []);

  const unitsRef = useRef(plan.units);
  unitsRef.current = plan.units;
  useEffect(() => {
    if (!mapReady) return;
    ctl.current?.setArea(plan.area?.polygons ?? null, fitNext.current);
    const first = unitsRef.current[0];
    if (fitNext.current && !plan.area && first) ctl.current?.map.setView([first.lat, first.lng], 14);
    fitNext.current = false;
  }, [plan.area, mapReady]);

  useEffect(() => {
    if (mapReady) ctl.current?.setUnits(plan.units, analysis?.labels ?? new Map(), showRanges);
  }, [plan.units, analysis, showRanges, mapReady]);

  useEffect(() => {
    if (mapReady) ctl.current?.highlightUnit(selectedId);
  }, [selectedId, plan.units, mapReady]);

  useEffect(() => {
    if (mapReady) ctl.current?.setCoverage(image?.url ?? null, image?.sw ?? null, image?.ne ?? null, opacity);
  }, [image, opacity, mapReady]);

  useEffect(() => {
    if (mapReady) ctl.current?.setLinks(analysis?.links ?? [], showLinks);
  }, [analysis, showLinks, mapReady]);

  const hhColours = useMemo(
    () => (analysis ? householdColours(analysis, view, plan.model.hopLimit) : []),
    [analysis, view, plan.model.hopLimit],
  );
  useEffect(() => {
    if (!mapReady) return;
    // Points and colours come from the same analysis, so they always match.
    const pts = analysis?.households ? (plan.householdPoints?.points ?? null) : null;
    ctl.current?.setHouseholds(showHouseholds && pts && pts.length === hhColours.length ? pts : null, hhColours);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [hhColours, showHouseholds, mapReady]);

  useEffect(() => {
    if (mapReady) ctl.current?.setMode(mode);
  }, [mode, mapReady]);

  useEffect(() => {
    if (mapReady) ctl.current?.showConstituencies(showAcOutlines && acs ? (acs as unknown as GeoJSON.FeatureCollection) : null);
  }, [showAcOutlines, acs, mapReady]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        setMode("pan");
        ctl.current?.clearProbe();
        ctl.current?.cancelDraw();
        setDrawing(null);
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  // Ctrl+P without the Print button: give the map its printed size (the
  // "printing" class, set on the DOM at once since a React update would come
  // too late) and fit the area into it. Tiles may not all load in time.
  const appEl = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const before = () => {
      appEl.current?.classList.add("printing");
      ctl.current?.invalidate();
      ctl.current?.fitArea();
    };
    const after = () => {
      appEl.current?.classList.remove("printing");
      setTimeout(() => ctl.current?.invalidate(), 100);
    };
    window.addEventListener("beforeprint", before);
    window.addEventListener("afterprint", after);
    return () => {
      window.removeEventListener("beforeprint", before);
      window.removeEventListener("afterprint", after);
    };
  }, []);

  const print = useCallback(() => {
    setPrinting(true);
    // Let the map take its print size and load tiles before the dialog opens.
    setTimeout(() => {
      ctl.current?.invalidate();
      ctl.current?.fitArea();
      setTimeout(() => {
        window.print();
        setPrinting(false);
        setTimeout(() => ctl.current?.invalidate(), 100);
      }, 1500);
    }, 100);
  }, []);

  // ------------------------------------------------------------- render

  const s = analysis?.coverage?.stats;
  const km2 = analysis?.areaKm2 ?? 0;
  const powered = plan.units.filter((u) => u.kind === "powered").length;
  const responders = plan.units.filter((u) => u.kind === "responder").length;
  const est = costs ? planEstimate(plan, analysis, costs) : null;

  return (
    <div ref={appEl} className={`app ${printing ? "printing" : ""}`}>
      <header className="topbar">
        <div className="brand">
          <strong>FloodMesh</strong> Coverage Planner
          <span className="areaTitle">{plan.area ? plan.area.name : "No area chosen"}</span>
        </div>
        <div className="kpis">
          <div>
            <span>Area</span>
            <strong>{km2 ? `${km2.toFixed(1)} km²` : "–"}</strong>
          </div>
          <div>
            <span>Household units</span>
            <strong title={demandPending(plan.demand) ? "Count the OSM buildings in the Units tab first" : undefined}>
              {demandPending(plan.demand) ? "count buildings" : fmtInt(householdUnits(plan.demand, km2))}
            </strong>
          </div>
          <div>
            <span>Powered / responders</span>
            <strong>
              {powered} / {responders + plan.extraResponders}
            </strong>
          </div>
          <div>
            <span>Hears ≥ 2</span>
            <strong>{s ? fmtPct(s.pctCovered2) : "–"}</strong>
          </div>
          <div>
            <span>SOS reach</span>
            <strong>{s?.hasResponders ? fmtPct(s.pctSosReachable) : "–"}</strong>
          </div>
          <div>
            <span>Estimate</span>
            <strong>{est ? formatInr(est.total) : "–"}</strong>
          </div>
        </div>
        <a className="adminLink" href="/admin">
          Admin
        </a>
      </header>

      <div className="main">
        <div className="mapPane">
          <div ref={mapEl} className="map" role="region" aria-label="Planning map" />
          <div className="mapTools" role="group" aria-label="Map tool">
            {QUICK_MODES.map((m) => (
              <button key={m.id} type="button" title={m.title} className={mode === m.id ? "on" : ""} onClick={() => setMode(m.id)} aria-pressed={mode === m.id}>
                {m.label}
              </button>
            ))}
            {plan.area && (
              <button type="button" title="Zoom to the area" onClick={() => ctl.current?.fitArea()}>
                Fit area
              </button>
            )}
          </div>
          {view !== "none" && analysis?.coverage && (
            <div className="mapLegend">
              <Legend view={view} hopLimit={plan.model.hopLimit} />
            </div>
          )}
          {!mapReady && <div className="mapLoading">Loading map…</div>}
        </div>

        <aside className="panel">
          <nav className="tabs" role="tablist">
            {TABS.map((t) => (
              <button key={t.id} type="button" role="tab" aria-selected={tab === t.id} className={tab === t.id ? "on" : ""} onClick={() => setTab(t.id)}>
                {t.label}
              </button>
            ))}
          </nav>
          <div className="panelBody">
            {tab === "area" && (
              <AreaPanel
                area={plan.area}
                areaKm2={km2}
                acs={acs}
                acsError={acsError}
                loadAcs={loadAcs}
                showAcOutlines={showAcOutlines}
                setShowAcOutlines={setShowAcOutlines}
                setArea={setArea}
                drawing={drawing}
                startDraw={(sh) => {
                  setMode("pan");
                  // Start the map's draw first: switching shapes ends the
                  // previous draw, whose "draw ended" must not win over this.
                  ctl.current?.startDraw(sh);
                  setDrawing(sh);
                }}
                cancelDraw={() => {
                  ctl.current?.cancelDraw();
                  setDrawing(null);
                }}
                editing={editing}
                toggleEdit={() => {
                  if (editing) ctl.current?.stopEditArea();
                  else ctl.current?.startEditArea();
                  setEditing(!editing);
                }}
                notify={notify}
              />
            )}
            {tab === "units" && (
              <UnitsPanel
                plan={plan}
                analysis={analysis}
                mode={mode}
                setMode={setMode}
                selectedId={selectedId}
                setSelectedId={setSelectedId}
                updateUnit={updateUnit}
                removeUnit={removeUnit}
                clearUnits={clearUnits}
                autoPlace={runAutoPlace}
                addResponderAtCentre={addResponderAtCentre}
                setDemand={setDemand}
                setPlanField={setPlanField}
                countBuildings={() => void doCountBuildings()}
                countingBuildings={counting}
                placeHouseholds={(src) => void placeHouseholds(src)}
                placingHouseholds={placing}
                clearHouseholds={clearHouseholds}
                panToUnit={(u) => ctl.current?.panToUnit(u)}
              />
            )}
            {tab === "coverage" && (
              <CoveragePanel
                plan={plan}
                analysis={analysis}
                view={view}
                setView={setView}
                opacity={opacity}
                setOpacity={setOpacity}
                showLinks={showLinks}
                setShowLinks={setShowLinks}
                showRanges={showRanges}
                setShowRanges={setShowRanges}
                showHouseholds={showHouseholds}
                setShowHouseholds={setShowHouseholds}
              />
            )}
            {tab === "cost" && <CostPanel plan={plan} analysis={analysis} costs={costs} costsError={costsError} />}
            {tab === "radio" && (
              <RadioPanel plan={plan} analysis={analysis} setModel={setModel} setEnv={setEnv} setProfile={setProfile} resetRadio={resetRadio} />
            )}
            {tab === "report" && <ReportPanel plan={plan} setReport={setReport} openPlan={(f) => void openPlan(f)} newPlan={newPlan} print={print} />}
          </div>
        </aside>
      </div>

      <PrintReport plan={plan} analysis={analysis} costs={costs} />

      {toast && (
        <div className="toast" role="status" onClick={() => setToast(null)}>
          {toast}
        </div>
      )}
    </div>
  );
}
