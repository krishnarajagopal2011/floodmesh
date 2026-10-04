/**
 * OpenStreetMap lookups from the browser: place search (Nominatim) and a
 * building count inside the area (Overpass).
 *
 * Both are free community services with usage policies: Nominatim allows at
 * most one request per second and wants the site identified, so searches run
 * only when the user presses Search, never per keystroke, and requests send
 * the site's origin as Referer. Heavy or bulk use needs a self-hosted
 * instance or a commercial provider.
 */
import type { LngLat, PolygonRings } from "./plan.ts";

export interface PlaceResult {
  id: string;
  name: string;
  display: string;
  kind: string;
  lat: number;
  lng: number;
  polygons: PolygonRings[];
}

const NOMINATIM = "https://nominatim.openstreetmap.org/search";
const OVERPASS = "https://overpass-api.de/api/interpreter";
/** Greater Chennai, for biasing search results (lon1,lat1,lon2,lat2). */
export const CHENNAI_VIEWBOX = "79.95,13.30,80.40,12.75";

let lastNominatim = 0;

function toPolygons(g: { type?: string; coordinates?: unknown } | undefined): PolygonRings[] {
  if (!g) return [];
  if (g.type === "Polygon") return [g.coordinates as PolygonRings];
  if (g.type === "MultiPolygon") return g.coordinates as PolygonRings[];
  return [];
}

export async function searchPlaces(q: string, nearChennai: boolean, signal?: AbortSignal): Promise<PlaceResult[]> {
  const wait = lastNominatim + 1100 - Date.now();
  if (wait > 0) await new Promise((r) => setTimeout(r, wait));
  lastNominatim = Date.now();
  const p = new URLSearchParams({
    q,
    format: "jsonv2",
    polygon_geojson: "1",
    polygon_threshold: "0.00005",
    limit: "8",
    countrycodes: "in",
    "accept-language": "en",
  });
  if (nearChennai) p.set("viewbox", CHENNAI_VIEWBOX);
  const res = await fetch(`${NOMINATIM}?${p}`, { signal, referrerPolicy: "strict-origin-when-cross-origin" });
  if (!res.ok) throw new Error(`Place search failed (HTTP ${res.status}).`);
  const rows = (await res.json()) as Array<{
    place_id: number;
    name?: string;
    display_name: string;
    type?: string;
    addresstype?: string;
    lat: string;
    lon: string;
    geojson?: { type?: string; coordinates?: unknown };
  }>;
  return rows.map((r) => ({
    id: String(r.place_id),
    name: r.name || r.display_name.split(",")[0],
    display: r.display_name,
    kind: r.addresstype || r.type || "place",
    lat: Number(r.lat),
    lng: Number(r.lon),
    polygons: toPolygons(r.geojson),
  }));
}

/** Outer ring as Overpass "lat lon lat lon ..." with at most `max` vertices. */
function polyString(ring: LngLat[], max = 250): string {
  const step = Math.max(1, Math.ceil(ring.length / max));
  const pts: string[] = [];
  for (let i = 0; i < ring.length; i += step) pts.push(`${ring[i][1].toFixed(6)} ${ring[i][0].toFixed(6)}`);
  return pts.join(" ");
}

/**
 * Buildings mapped in OSM inside the area (outer rings only; holes are
 * ignored). OSM coverage of buildings in Indian cities is incomplete, so the
 * planner treats this as a lower bound and lets the user scale it.
 */
export async function countBuildings(polygons: PolygonRings[], signal?: AbortSignal): Promise<number> {
  const parts = polygons
    .filter((p) => p[0] && p[0].length >= 4)
    .map((p) => {
      const s = polyString(p[0]);
      return `way["building"](poly:"${s}");relation["building"](poly:"${s}");`;
    })
    .join("");
  if (!parts) return 0;
  const q = `[out:json][timeout:120];(${parts});out count;`;
  const res = await fetch(OVERPASS, {
    method: "POST",
    body: new URLSearchParams({ data: q }),
    signal,
    referrerPolicy: "strict-origin-when-cross-origin",
  });
  if (!res.ok) throw new Error(`Building count failed (HTTP ${res.status}). Overpass may be busy; try again in a minute.`);
  const j = (await res.json()) as { elements?: Array<{ tags?: Record<string, string> }> };
  const total = Number(j.elements?.[0]?.tags?.total ?? NaN);
  if (!Number.isFinite(total)) throw new Error("Building count: unexpected reply from Overpass.");
  return total;
}

/**
 * Positions (centres) of the buildings mapped in OSM inside the area, for
 * placing household units on real buildings. Outer rings only. A large
 * constituency can return tens of thousands of points (a few MB).
 */
export async function buildingCentres(polygons: PolygonRings[], signal?: AbortSignal): Promise<LngLat[]> {
  const parts = polygons
    .filter((p) => p[0] && p[0].length >= 4)
    .map((p) => {
      const s = polyString(p[0]);
      return `way["building"](poly:"${s}");relation["building"](poly:"${s}");`;
    })
    .join("");
  if (!parts) return [];
  const q = `[out:json][timeout:180];(${parts});out ids center qt;`;
  const res = await fetch(OVERPASS, {
    method: "POST",
    body: new URLSearchParams({ data: q }),
    signal,
    referrerPolicy: "strict-origin-when-cross-origin",
  });
  if (!res.ok) throw new Error(`Building positions failed (HTTP ${res.status}). Overpass may be busy; try again in a minute.`);
  const j = (await res.json()) as { elements?: Array<{ center?: { lat: number; lon: number } }> };
  const out: LngLat[] = [];
  for (const e of j.elements ?? []) {
    if (e.center && Number.isFinite(e.center.lat) && Number.isFinite(e.center.lon)) out.push([e.center.lon, e.center.lat]);
  }
  return out;
}
