"use client";
/**
 * Map of each unit's latest phone position (Leaflet, OpenStreetMap tiles).
 *
 * Leaflet touches `window` when imported, so it is loaded in an effect,
 * never during server rendering. Markers are lettered divIcons, which also
 * avoids Leaflet's default marker images that bundlers fail to resolve.
 */
import { useEffect, useRef } from "react";
import type { Map as LeafletMap, LayerGroup } from "leaflet";
import "leaflet/dist/leaflet.css";
import { age, clock } from "./format";

export interface MapPoint {
  unit: string;
  lat: number;
  lon: number;
  accuracy: number | null;
  ts: string | null;
}

// Chennai, where the tests run, until the first position arrives.
const DEFAULT_CENTER: [number, number] = [13.0827, 80.2707];

function esc(s: string): string {
  return s.replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
}

export default function UnitMap({ points }: { points: MapPoint[] }) {
  const el = useRef<HTMLDivElement>(null);
  const map = useRef<LeafletMap | null>(null);
  const layer = useRef<LayerGroup | null>(null);
  const fitted = useRef(false);
  const L = useRef<typeof import("leaflet") | null>(null);
  const latest = useRef(points);
  latest.current = points;

  function draw() {
    const lf = L.current;
    if (!lf || !map.current || !layer.current) return;
    layer.current.clearLayers();
    const now = Date.now();
    for (const p of latest.current) {
      if (p.accuracy !== null && p.accuracy > 0) {
        lf.circle([p.lat, p.lon], { radius: p.accuracy, color: "#003f9e", weight: 2, fillOpacity: 0.08 }).addTo(
          layer.current,
        );
      }
      const icon = lf.divIcon({ className: "unitMarker", html: esc(p.unit), iconSize: [32, 32] });
      lf.marker([p.lat, p.lon], { icon, title: `Unit ${p.unit}` })
        .bindPopup(
          `<strong>Unit ${esc(p.unit)}</strong><br>${esc(clock(p.ts))} (${esc(age(p.ts, now))})` +
            (p.accuracy !== null ? `<br>±${Math.round(p.accuracy)} m` : ""),
        )
        .addTo(layer.current);
    }
    if (!fitted.current && latest.current.length > 0) {
      fitted.current = true;
      fitAll();
    }
  }

  function fitAll() {
    const lf = L.current;
    if (!lf || !map.current || latest.current.length === 0) return;
    const bounds = lf.latLngBounds(latest.current.map((p) => [p.lat, p.lon] as [number, number]));
    map.current.fitBounds(bounds.pad(0.3), { maxZoom: 17 });
  }

  useEffect(() => {
    let cancelled = false;
    (async () => {
      const lf = await import("leaflet");
      if (cancelled || !el.current || map.current) return;
      L.current = lf;
      // Wheel zoom off: scrolling the page must not get caught by the map.
      map.current = lf.map(el.current, { scrollWheelZoom: false }).setView(DEFAULT_CENTER, 12);
      lf.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
        maxZoom: 19,
        attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
        // OSM's tile policy blocks requests without a Referer, and the site-wide
        // Referrer-Policy (next.config.ts) is same-origin. For tiles only, send
        // the site's origin (never page paths) so OSM can identify the app.
        referrerPolicy: "strict-origin-when-cross-origin",
      }).addTo(map.current);
      layer.current = lf.layerGroup().addTo(map.current);
      draw();
    })();
    return () => {
      cancelled = true;
      map.current?.remove();
      map.current = null;
      layer.current = null;
    };
  }, []);

  useEffect(() => {
    draw();
  }, [points]);

  return (
    <>
      <div ref={el} className="map" role="region" aria-label="Map of phone positions" />
      <div className="row">
        <button type="button" className="secondary" onClick={fitAll} disabled={points.length === 0}>
          Show all units
        </button>
        {points.length === 0 && <span className="muted">No positions shared yet.</span>}
      </div>
    </>
  );
}
