/**
 * Everything that touches Leaflet. The planner component owns the state and
 * calls these methods; the map reports user actions back through callbacks.
 *
 * Browser only: Leaflet reads `window` when imported, so this module is
 * loaded with a dynamic import from an effect. Leaflet-Geoman (drawing and
 * shape editing) is an IIFE that patches the global `L`, so Leaflet is put
 * on `window` before Geoman is imported.
 */
import type * as Leaflet from "leaflet";
import type { LngLat, PlacedUnit, PolygonRings } from "@/lib/plan";

type LT = typeof Leaflet;

export type MapMode = "pan" | "add-powered" | "add-responder" | "probe";
export type DrawShape = "Polygon" | "Rectangle" | "Circle";

export interface MapCallbacks {
  onAddUnit(kind: PlacedUnit["kind"], lat: number, lng: number): void;
  onMoveUnit(id: string, lat: number, lng: number): void;
  onSelectUnit(id: string): void;
  onProbe(lat: number, lng: number): void;
  onDrawn(polygons: PolygonRings[]): void;
  onDrawEnd(): void;
  onAreaEdited(polygons: PolygonRings[]): void;
  onPickConstituency(acNo: number): void;
}

export interface UnitLabel {
  id: string;
  rangeM: number;
}

export interface LinkLine {
  a: [number, number];
  b: [number, number];
  marginDb: number;
}

const COLORS = {
  area: "#0b4f9c",
  powered: "#0b4f9c",
  responder: "#b42318",
  linkGood: "#1a7f37",
  linkWeak: "#b7791f",
};

function esc(s: string): string {
  return s.replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
}

export async function createMap(el: HTMLElement, cb: MapCallbacks): Promise<MapController> {
  const mod = await import("leaflet");
  const L = ((mod as unknown as { default?: LT }).default ?? mod) as LT;
  (window as unknown as { L: LT }).L = L;
  await import("@geoman-io/leaflet-geoman-free");
  return new MapController(L, el, cb);
}

export class MapController {
  readonly map: Leaflet.Map;
  private L: LT;
  private cb: MapCallbacks;
  private mode: MapMode = "pan";
  private areaLayer: Leaflet.GeoJSON;
  private acLayer: Leaflet.GeoJSON | null = null;
  private unitLayer: Leaflet.LayerGroup;
  private rangeLayer: Leaflet.LayerGroup;
  private linkLayer: Leaflet.LayerGroup;
  private overlay: Leaflet.ImageOverlay | null = null;
  private probeLayer: Leaflet.LayerGroup;
  private householdLayer: Leaflet.LayerGroup;
  private householdRenderer: Leaflet.Canvas;
  private householdData: { points: LngLat[]; colours: string[] } | null = null;
  /** Dot size and outline last drawn, so a zoom redraws the dots only when they change. */
  private householdStyle = "";
  private markers = new Map<string, Leaflet.Marker>();
  private units: PlacedUnit[] = [];
  private selectedId: string | null = null;
  private iconScale: IconScale = "full";
  private editing = false;
  /** Set by Geoman's edit events while the area is being edited. */
  private areaDirty = false;

  constructor(L: LT, el: HTMLElement, cb: MapCallbacks) {
    this.L = L;
    this.cb = cb;
    this.map = L.map(el, { zoomControl: true, preferCanvas: false }).setView([13.05, 80.21], 11);

    // OSM's tile policy needs a Referer; the site-wide policy is same-origin,
    // so tiles send just the site's origin (never page paths).
    const tilePolicy = { referrerPolicy: "strict-origin-when-cross-origin" as const };
    const osm = L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
      maxZoom: 19,
      attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
      ...tilePolicy,
    });
    const sat = L.tileLayer(
      "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
      {
        maxZoom: 19,
        attribution: "Imagery &copy; Esri, Maxar, Earthstar Geographics, and the GIS User Community",
          ...tilePolicy,
      },
    );
    const light = L.tileLayer("https://{s}.basemaps.cartocdn.com/light_all/{z}/{x}/{y}{r}.png", {
      maxZoom: 20,
      subdomains: "abcd",
      attribution:
        '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors &copy; <a href="https://carto.com/attributions">CARTO</a>',
      ...tilePolicy,
    });
    osm.addTo(this.map);
    L.control.layers({ "OpenStreetMap": osm, "Satellite (Esri)": sat, "Light (CARTO)": light }, {}, { position: "topright" }).addTo(this.map);
    L.control.scale({ metric: true, imperial: false }).addTo(this.map);

    this.map.createPane("coverage");
    this.map.getPane("coverage")!.style.zIndex = "350";
    this.map.createPane("links");
    this.map.getPane("links")!.style.zIndex = "420";

    this.areaLayer = L.geoJSON(undefined, {
      style: { color: COLORS.area, weight: 3, fill: false, dashArray: "6 4" },
      pmIgnore: false,
    } as Leaflet.GeoJSONOptions).addTo(this.map);
    this.rangeLayer = L.layerGroup().addTo(this.map);
    this.linkLayer = L.layerGroup().addTo(this.map);
    this.unitLayer = L.layerGroup().addTo(this.map);
    this.probeLayer = L.layerGroup().addTo(this.map);
    // Thousands of household dots: one canvas, not thousands of SVG elements.
    this.map.createPane("households");
    this.map.getPane("households")!.style.zIndex = "430";
    this.map.getPane("households")!.style.pointerEvents = "none";
    this.householdRenderer = L.canvas({ pane: "households", padding: 0.3 });
    this.householdLayer = L.layerGroup().addTo(this.map);

    const pm = (this.map as unknown as { pm: PMMap }).pm;
    pm.setGlobalOptions({ snappable: false, continueDrawing: false, exitModeOnEscape: true, templineStyle: { color: COLORS.area }, hintlineStyle: { color: COLORS.area, dashArray: "5 5" }, pathOptions: { color: COLORS.area } });

    this.map.on("pm:create", (e: Leaflet.LeafletEvent) => {
      const ev = e as Leaflet.LeafletEvent & { layer: Leaflet.Layer; shape: string };
      const polys = layerToPolygons(this.L, ev.layer);
      this.map.removeLayer(ev.layer);
      if (polys.length) this.cb.onDrawn(polys);
    });

    this.map.on("pm:drawend", () => {
      this.map.getContainer().classList.remove("drawing");
      this.cb.onDrawEnd();
    });

    // A constituency can hold hundreds of units: draw them as dots until zoomed in.
    this.iconScale = scaleFor(this.map.getZoom());
    this.map.on("zoomend", () => {
      if (householdStyleFor(this.map.getZoom()).key !== this.householdStyle) this.drawHouseholds();
      const sc = scaleFor(this.map.getZoom());
      if (sc === this.iconScale) return;
      this.iconScale = sc;
      for (const u of this.units) this.markers.get(u.id)?.setIcon(this.icon(u));
      this.highlightUnit(this.selectedId);
    });

    this.map.on("click", (e: Leaflet.LeafletMouseEvent) => {
      if (pm.globalDrawModeEnabled()) return;
      if (this.mode === "add-powered") this.cb.onAddUnit("powered", e.latlng.lat, e.latlng.lng);
      else if (this.mode === "add-responder") this.cb.onAddUnit("responder", e.latlng.lat, e.latlng.lng);
      else if (this.mode === "probe") this.cb.onProbe(e.latlng.lat, e.latlng.lng);
    });
  }

  destroy(): void {
    this.map.remove();
  }

  invalidate(): void {
    this.map.invalidateSize();
  }

  setMode(mode: MapMode): void {
    this.mode = mode;
    const el = this.map.getContainer();
    el.style.cursor = mode === "pan" ? "" : "crosshair";
    // Probing goes through unit markers, so a dense plan can still be probed anywhere.
    el.classList.toggle("clickThrough", mode === "probe");
    if (mode !== "probe") this.probeLayer.clearLayers();
  }

  startDraw(shape: DrawShape): void {
    this.setMode("pan");
    const pm = (this.map as unknown as { pm: PMMap }).pm;
    // End any draw in progress first: its pm:drawend (which clears the
    // "drawing" class and the planner's draw state) must come before, not
    // after, this draw starts.
    pm.disableDraw();
    // Corners may fall on unit markers; let the clicks through to the map.
    this.map.getContainer().classList.add("drawing");
    pm.enableDraw(shape, { pathOptions: { color: COLORS.area } });
  }

  cancelDraw(): void {
    this.map.getContainer().classList.remove("drawing");
    (this.map as unknown as { pm: PMMap }).pm.disableDraw();
  }

  // -------------------------------------------------------------- area

  setArea(polygons: PolygonRings[] | null, fit: boolean): void {
    this.stopEditArea(false);
    this.areaLayer.clearLayers();
    if (!polygons || polygons.length === 0) return;
    this.areaLayer.addData({
      type: "Feature",
      properties: {},
      geometry: { type: "MultiPolygon", coordinates: polygons },
    } as GeoJSON.Feature);
    if (fit) this.fitArea();
  }

  fitArea(): void {
    const b = this.areaLayer.getBounds();
    if (b.isValid()) this.map.fitBounds(b, { padding: [24, 24] });
  }

  get isEditingArea(): boolean {
    return this.editing;
  }

  startEditArea(): void {
    if (this.editing) return;
    this.editing = true;
    this.areaDirty = false;
    this.areaLayer.eachLayer((l) => {
      const layer = l as Leaflet.Polygon & { pm?: { enable(o?: object): void } };
      // Adjacent constituencies share border vertices, which Geoman's
      // self-intersection check counts as crossings: with the check on, every
      // drag on a multi-part area would snap back. Keep it for single shapes.
      const gj = layer.toGeoJSON?.();
      const multiPart = gj?.geometry.type === "MultiPolygon" && gj.geometry.coordinates.length > 1;
      layer.on("pm:edit pm:vertexadded pm:vertexremoved", this.markDirty);
      layer.pm?.enable({ allowSelfIntersection: multiPart });
    });
  }

  private markDirty = () => {
    this.areaDirty = true;
  };

  /** Ends editing; with `report`, hands the edited shape back to the planner. */
  stopEditArea(report = true): void {
    if (!this.editing) return;
    this.editing = false;
    const polys: PolygonRings[] = [];
    this.areaLayer.eachLayer((l) => {
      const layer = l as Leaflet.Layer & { pm?: { disable(): void } };
      layer.pm?.disable();
      layer.off("pm:edit pm:vertexadded pm:vertexremoved", this.markDirty);
      polys.push(...layerToPolygons(this.L, layer));
    });
    // "Finish editing" without a change keeps the area as it was (a
    // constituency stays a constituency).
    if (report && this.areaDirty && polys.length) this.cb.onAreaEdited(polys);
    this.areaDirty = false;
  }

  /** All constituencies as faint outlines; a click picks one. */
  showConstituencies(fc: GeoJSON.FeatureCollection | null): void {
    if (this.acLayer) {
      this.map.removeLayer(this.acLayer);
      this.acLayer = null;
    }
    if (!fc) return;
    this.acLayer = this.L.geoJSON(fc, {
      pmIgnore: true,
      style: { color: "#6b4fa0", weight: 1, opacity: 0.7, fillOpacity: 0.02 },
      onEachFeature: (f, layer) => {
        const p = f.properties as { ac_no: number; name: string; district: string };
        layer.bindTooltip(`${esc(p.name)} (AC ${p.ac_no})`, { sticky: true });
        layer.on("click", (e: Leaflet.LeafletMouseEvent) => {
          // While drawing or editing, the click must reach the map (Geoman
          // builds shapes from map clicks), so don't stop it here.
          const pm = (this.map as unknown as { pm: PMMap }).pm;
          if (this.mode !== "pan" || this.editing || pm.globalDrawModeEnabled()) return;
          this.L.DomEvent.stopPropagation(e);
          this.cb.onPickConstituency(p.ac_no);
        });
      },
    } as Leaflet.GeoJSONOptions).addTo(this.map);
    this.acLayer.bringToBack();
  }

  // ------------------------------------------------------------- units

  private icon(u: PlacedUnit): Leaflet.DivIcon {
    const sc = this.iconScale;
    const size = sc === "full" ? (u.kind === "powered" ? 30 : 28) : sc === "small" ? 14 : 9;
    return this.L.divIcon({
      className: `unitIcon ${u.kind} ${sc}`,
      html: sc === "full" ? `<span>${esc(u.name)}</span>` : "",
      iconSize: [size, size],
    });
  }

  setUnits(units: PlacedUnit[], labels: Map<string, UnitLabel>, showRanges: boolean): void {
    const L = this.L;
    this.units = units;
    const seen = new Set<string>();
    for (const u of units) {
      seen.add(u.id);
      const icon = this.icon(u);
      let m = this.markers.get(u.id);
      if (!m) {
        m = L.marker([u.lat, u.lng], { icon, draggable: true, title: u.name, pmIgnore: true } as Leaflet.MarkerOptions);
        const id = u.id;
        m.on("dragend", () => {
          const p = m!.getLatLng();
          this.cb.onMoveUnit(id, p.lat, p.lng);
        });
        m.on("click", (e: Leaflet.LeafletMouseEvent) => {
          L.DomEvent.stopPropagation(e);
          this.cb.onSelectUnit(id);
        });
        m.addTo(this.unitLayer);
        this.markers.set(u.id, m);
      } else {
        const p = m.getLatLng();
        if (p.lat !== u.lat || p.lng !== u.lng) m.setLatLng([u.lat, u.lng]);
        m.options.title = u.name;
        const el = m.getElement();
        if (!el || el.textContent !== (this.iconScale === "full" ? u.name : "")) m.setIcon(icon);
        // Leaflet copies the title only when it creates the icon element, and
        // at dot scale the element is not replaced on a rename: set it here.
        const el2 = m.getElement();
        if (el2 && el2.title !== u.name) el2.title = u.name;
      }
    }
    for (const [id, m] of this.markers) {
      if (!seen.has(id)) {
        this.unitLayer.removeLayer(m);
        this.markers.delete(id);
      }
    }

    this.rangeLayer.clearLayers();
    if (showRanges) {
      for (const u of units) {
        const r = labels.get(u.id)?.rangeM ?? 0;
        if (r <= 0) continue;
        L.circle([u.lat, u.lng], {
          radius: r,
          color: u.kind === "powered" ? COLORS.powered : COLORS.responder,
          weight: 1,
          dashArray: "4 4",
          fill: false,
          interactive: false,
          pmIgnore: true,
        } as Leaflet.CircleOptions).addTo(this.rangeLayer);
      }
    }
  }

  highlightUnit(id: string | null): void {
    this.selectedId = id;
    for (const [uid, m] of this.markers) {
      m.getElement()?.classList.toggle("selected", uid === id);
    }
  }

  panToUnit(u: PlacedUnit): void {
    this.map.panTo([u.lat, u.lng]);
  }

  // --------------------------------------------------------- households

  /** Placed household units as small dots, coloured per unit; null hides them. */
  setHouseholds(points: LngLat[] | null, colours: string[]): void {
    this.householdData = points && points.length ? { points, colours } : null;
    this.drawHouseholds();
  }

  private drawHouseholds(): void {
    this.householdLayer.clearLayers();
    const d = this.householdData;
    const st = householdStyleFor(this.map.getZoom());
    this.householdStyle = st.key;
    if (!d) return;
    const { radius, weight } = st;
    for (let i = 0; i < d.points.length; i++) {
      const [lng, lat] = d.points[i];
      this.L.circleMarker([lat, lng], {
        renderer: this.householdRenderer,
        pane: "households",
        radius,
        // A dark rim keeps each dot readable on top of the same-coloured coverage layer.
        stroke: true,
        color: "rgba(17,24,39,0.75)",
        weight,
        fillColor: d.colours[i] ?? "#374151",
        fillOpacity: 0.9,
        interactive: false,
        pmIgnore: true,
      } as Leaflet.CircleMarkerOptions).addTo(this.householdLayer);
    }
  }

  // ------------------------------------------------------ links/coverage

  setLinks(lines: LinkLine[], show: boolean): void {
    this.linkLayer.clearLayers();
    if (!show) return;
    for (const l of lines) {
      this.L.polyline([l.a, l.b], {
        pane: "links",
        color: l.marginDb >= 10 ? COLORS.linkGood : COLORS.linkWeak,
        weight: 2,
        opacity: 0.8,
        interactive: false,
        pmIgnore: true,
      } as Leaflet.PolylineOptions).addTo(this.linkLayer);
    }
  }

  setCoverage(dataUrl: string | null, sw: LngLat | null, ne: LngLat | null, opacity: number): void {
    if (this.overlay) {
      this.map.removeLayer(this.overlay);
      this.overlay = null;
    }
    if (!dataUrl || !sw || !ne) return;
    this.overlay = this.L.imageOverlay(
      dataUrl,
      [
        [sw[1], sw[0]],
        [ne[1], ne[0]],
      ],
      { pane: "coverage", opacity, interactive: false, className: "coverageImage", pmIgnore: true } as Leaflet.ImageOverlayOptions,
    ).addTo(this.map);
  }

  showProbe(lat: number, lng: number, html: string, lines: LinkLine[]): void {
    this.probeLayer.clearLayers();
    for (const l of lines) {
      this.L.polyline([l.a, l.b], {
        color: l.marginDb >= 0 ? "#111" : "#b42318",
        weight: 2,
        dashArray: "2 6",
        interactive: false,
        pmIgnore: true,
      } as Leaflet.PolylineOptions).addTo(this.probeLayer);
    }
    this.L.circleMarker([lat, lng], { radius: 6, color: "#111", fillColor: "#fff", fillOpacity: 1, weight: 2, pmIgnore: true } as Leaflet.CircleMarkerOptions)
      .addTo(this.probeLayer)
      .bindPopup(html, { maxWidth: 360, autoPan: true })
      .openPopup();
  }

  clearProbe(): void {
    this.probeLayer.clearLayers();
  }
}

// ---------------------------------------------------------------- helpers

type IconScale = "full" | "small" | "dot";

function householdStyleFor(zoom: number) {
  const radius = zoom >= 16 ? 4 : zoom >= 14 ? 3 : 2;
  const weight = zoom >= 15 ? 1.2 : 0.6;
  return { radius, weight, key: `${radius}/${weight}` };
}

function scaleFor(zoom: number): IconScale {
  return zoom >= 15 ? "full" : zoom >= 13 ? "small" : "dot";
}

interface PMMap {
  setGlobalOptions(o: object): void;
  enableDraw(shape: string, o?: object): void;
  disableDraw(): void;
  globalDrawModeEnabled(): boolean;
}

/** GeoJSON polygons of a drawn or edited layer. A circle becomes a 72-sided polygon. */
export function layerToPolygons(L: LT, layer: Leaflet.Layer): PolygonRings[] {
  if (layer instanceof L.Circle) {
    const c = layer.getLatLng();
    return [circleRings(c.lat, c.lng, layer.getRadius())];
  }
  const gj = (layer as Leaflet.Polygon).toGeoJSON?.();
  if (!gj) return [];
  return geometryToPolygons(gj.geometry);
}

export function circleRings(lat: number, lng: number, radiusM: number, steps = 72): PolygonRings {
  const ring: LngLat[] = [];
  const kx = 111_320 * Math.cos((lat * Math.PI) / 180);
  for (let i = 0; i <= steps; i++) {
    const a = (2 * Math.PI * i) / steps;
    ring.push([lng + (radiusM * Math.cos(a)) / kx, lat + (radiusM * Math.sin(a)) / 110_540]);
  }
  return [ring];
}

export function geometryToPolygons(g: GeoJSON.Geometry | null | undefined): PolygonRings[] {
  if (!g) return [];
  if (g.type === "Polygon") return [g.coordinates as PolygonRings];
  if (g.type === "MultiPolygon") return g.coordinates as PolygonRings[];
  if (g.type === "GeometryCollection") return g.geometries.flatMap(geometryToPolygons);
  return [];
}
