/** Plan exports for other tools: GeoJSON (QGIS), KML (Google Earth) and CSV (spreadsheets). */
import type { PlacedUnit, PolygonRings } from "./plan.ts";
import { UNIT_LABELS } from "./radio.ts";

function xml(s: string): string {
  return s.replace(/[<>&'"]/g, (c) => ({ "<": "&lt;", ">": "&gt;", "&": "&amp;", "'": "&apos;", '"': "&quot;" })[c]!);
}

export function toGeoJson(areaName: string, polygons: PolygonRings[], units: PlacedUnit[]): string {
  const features: object[] = [];
  if (polygons.length) {
    features.push({
      type: "Feature",
      properties: { role: "area", name: areaName },
      geometry: { type: "MultiPolygon", coordinates: polygons },
    });
  }
  for (const u of units) {
    features.push({
      type: "Feature",
      properties: { role: u.kind, name: u.name, heightM: u.heightM ?? null, antennaDbi: u.antennaDbi ?? null },
      geometry: { type: "Point", coordinates: [u.lng, u.lat] },
    });
  }
  return JSON.stringify({ type: "FeatureCollection", features }, null, 1);
}

export function toKml(areaName: string, polygons: PolygonRings[], units: PlacedUnit[]): string {
  const ring = (r: [number, number][]) => r.map(([x, y]) => `${x},${y},0`).join(" ");
  const polys = polygons
    .map(
      (p) =>
        `<Polygon><outerBoundaryIs><LinearRing><coordinates>${ring(p[0])}</coordinates></LinearRing></outerBoundaryIs>` +
        p
          .slice(1)
          .map((h) => `<innerBoundaryIs><LinearRing><coordinates>${ring(h)}</coordinates></LinearRing></innerBoundaryIs>`)
          .join("") +
        `</Polygon>`,
    )
    .join("");
  const marks = units
    .map(
      (u) =>
        `<Placemark><name>${xml(u.name)}</name><description>${xml(UNIT_LABELS[u.kind])}${u.heightM ? `, ${u.heightM} m` : ""}</description>` +
        `<styleUrl>#${u.kind}</styleUrl><Point><coordinates>${u.lng},${u.lat},0</coordinates></Point></Placemark>`,
    )
    .join("\n");
  return `<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2"><Document><name>${xml(areaName)} - FloodMesh plan</name>
<Style id="area"><LineStyle><color>ff9c4f0b</color><width>3</width></LineStyle><PolyStyle><color>209c4f0b</color></PolyStyle></Style>
<Style id="powered"><IconStyle><color>ff9c4f0b</color><Icon><href>http://maps.google.com/mapfiles/kml/paddle/blu-square.png</href></Icon></IconStyle></Style>
<Style id="responder"><IconStyle><color>ff1823b4</color><Icon><href>http://maps.google.com/mapfiles/kml/paddle/red-circle.png</href></Icon></IconStyle></Style>
${polys ? `<Placemark><name>${xml(areaName)}</name><styleUrl>#area</styleUrl><MultiGeometry>${polys}</MultiGeometry></Placemark>` : ""}
${marks}
</Document></kml>
`;
}

export function toCsv(units: PlacedUnit[], defaults: Record<PlacedUnit["kind"], { heightM: number; antennaDbi: number }>): string {
  const q = (s: string) => (/[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s);
  const rows = ["name,type,lat,lng,height_m,antenna_dbi"];
  for (const u of units) {
    rows.push(
      [
        q(u.name),
        q(UNIT_LABELS[u.kind]),
        u.lat.toFixed(6),
        u.lng.toFixed(6),
        String(u.heightM ?? defaults[u.kind].heightM),
        String(u.antennaDbi ?? defaults[u.kind].antennaDbi),
      ].join(","),
    );
  }
  return rows.join("\n") + "\n";
}

export function download(filename: string, text: string, type: string): void {
  const blob = new Blob([text], { type });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

export function slug(s: string): string {
  return (
    s
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, "-")
      .replace(/^-|-$/g, "")
      .replace(/^floodmesh-/, "")
      .slice(0, 60) || "plan"
  );
}
