"use client";
/**
 * Test lead's view: which units are uploading, their battery and phone
 * positions, and the CSV exports for the analysis. Polls every 15 s.
 */
import { useCallback, useEffect, useState } from "react";
import type { UnitOverview } from "@/lib/queries";
import { age, clock, dayClock, fixed, isStale } from "./format";
import UnitMap, { type MapPoint } from "./UnitMap";

const REFRESH_MS = 15_000;

const EXPORTS = [
  { file: "readings_with_location.csv", label: "Readings with location", hint: "every rx with both phones' positions and distance" },
  { file: "records.csv", label: "Records", hint: "everything the units logged" },
  { file: "locations.csv", label: "Locations", hint: "every shared phone position" },
  { file: "notes.csv", label: "Notes", hint: "volunteers' notes" },
  { file: "conflicts.csv", label: "Seq collisions", hint: "records that reused a seq already stored, with the board's MAC" },
];

export default function AdminDashboard({ units }: { units: string[] }) {
  const [rows, setRows] = useState<UnitOverview[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loggedOut, setLoggedOut] = useState(false);
  const [now, setNow] = useState(() => Date.now());
  const [exportUnit, setExportUnit] = useState("");
  const [fetchedAt, setFetchedAt] = useState<number | null>(null);

  const load = useCallback(async () => {
    try {
      const res = await fetch("/api/overview", { cache: "no-store" });
      if (res.status === 401 || res.status === 403) {
        setLoggedOut(true);
        return;
      }
      const body = (await res.json()) as { ok: boolean; error?: string; units: UnitOverview[] };
      if (!res.ok || !body.ok) throw new Error(body.error ?? `HTTP ${res.status}`);
      setRows(body.units);
      setError(null);
      setFetchedAt(Date.now());
    } catch (err) {
      setError(`Could not refresh: ${(err as Error).message}. Retrying.`);
    }
  }, []);

  useEffect(() => {
    load();
    const poll = setInterval(load, REFRESH_MS);
    const tick = setInterval(() => setNow(Date.now()), 5_000);
    return () => {
      clearInterval(poll);
      clearInterval(tick);
    };
  }, [load]);

  const points: MapPoint[] = (rows ?? [])
    .filter((r) => r.location && r.location.lat !== null && r.location.lon !== null)
    .map((r) => ({
      unit: r.unit,
      lat: r.location!.lat as number,
      lon: r.location!.lon as number,
      accuracy: r.location!.accuracy,
      ts: r.location!.ts,
    }));

  const q = exportUnit ? `?unit=${encodeURIComponent(exportUnit)}` : "";
  // A unit whose seq restarted (flash erased) or whose call sign moved to
  // another board: its new records are kept apart, not in records.csv, and
  // the volunteer page may show the old board's latest records.
  const flagged = (rows ?? []).filter((r) => r.boards.length > 1 || r.conflicts > 0);

  return (
    <main>
      <div className="top">
        <div>
          <h1>All units</h1>
          <small>FloodMesh field test · admin</small>
        </div>
        <form method="post" action="/api/logout">
          <button type="submit" className="secondary">
            Log out
          </button>
        </form>
      </div>

      {loggedOut && (
        <p className="error" role="alert">
          Your login has expired. <a href="/login?role=admin">Log in again</a>.
        </p>
      )}
      {error && !loggedOut && <p className="error">{error}</p>}

      {flagged.length > 0 && (
        <section className="card warn" role="alert">
          <h2>Check these units</h2>
          <ul className="plain">
            {flagged.map((r) => (
              <li key={r.unit}>
                <strong>Unit {r.unit}:</strong>{" "}
                {r.boards.length > 1 && <>{r.boards.length} boards have uploaded as this unit ({r.boards.join(", ")}). </>}
                {r.conflicts > 0 && (
                  <>
                    {r.conflicts} record{r.conflicts === 1 ? "" : "s"} reused a sequence number already stored (board
                    swapped or its flash erased). They are kept in <code>conflicts.csv</code>, not in{" "}
                    <code>records.csv</code>.
                  </>
                )}
              </li>
            ))}
          </ul>
        </section>
      )}

      <section className="card">
        <h2>At a glance</h2>
        <div className="tableWrap">
          <table>
            <thead>
              <tr>
                <th>Unit</th>
                <th>Last upload</th>
                <th>fw / image</th>
                <th className="num">Battery</th>
                <th className="num">Today</th>
                <th className="num">Total</th>
                <th className="num">Lost</th>
                <th>Phone position</th>
              </tr>
            </thead>
            <tbody>
              {(rows ?? []).map((r) => {
                const stale = isStale(r.upload?.received_at, now);
                const locStale = isStale(r.location?.ts, now);
                return (
                  <tr key={r.unit}>
                    <td>
                      <a href={`/unit?u=${encodeURIComponent(r.unit)}`} style={{ fontSize: "1.3rem" }}>
                        {r.unit}
                      </a>
                    </td>
                    <td className={stale ? "badText" : "okText"}>
                      {r.upload ? (
                        <>
                          {dayClock(r.upload.received_at)}
                          <br />
                          {age(r.upload.received_at, now)}
                        </>
                      ) : (
                        "never"
                      )}
                    </td>
                    <td>
                      {r.upload?.fw ?? "—"}
                      <br />
                      <span className="muted">{r.upload?.image ?? ""}</span>
                    </td>
                    <td className="num">
                      {r.battery ? (
                        <>
                          {r.battery.batt_v !== null ? `${fixed(r.battery.batt_v, 2)} V` : "?"}
                          <br />
                          <span className="muted">
                            {r.battery.batt_pct !== null ? `${r.battery.batt_pct}%` : ""}
                            {r.battery.powered ? " ext" : ""}
                          </span>
                        </>
                      ) : (
                        "—"
                      )}
                    </td>
                    <td className="num">{r.records_today}</td>
                    <td className="num">{r.records_all}</td>
                    <td className={`num ${r.upload?.dropped ? "badText" : ""}`}>{r.upload?.dropped ?? "—"}</td>
                    <td className={r.location ? (locStale ? "badText" : "okText") : "muted"}>
                      {r.location ? (
                        <>
                          {age(r.location.ts, now)}
                          <br />
                          <span className="muted">
                            {r.location.accuracy !== null ? `±${Math.round(r.location.accuracy)} m` : ""}
                          </span>
                        </>
                      ) : (
                        "not shared"
                      )}
                    </td>
                  </tr>
                );
              })}
              {!rows && (
                <tr>
                  <td colSpan={8}>Loading…</td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
        <p className="muted">
          Red: no upload (or no position) for over 10 minutes.{" "}
          {fetchedAt ? `Updated ${clock(new Date(fetchedAt).toISOString())}, refreshes every 15 s.` : ""}
        </p>
      </section>

      <section className="card">
        <h2>Latest phone positions</h2>
        <UnitMap points={points} />
        <p className="muted">Circles show each phone&apos;s GPS accuracy. Positions only exist while a volunteer shares them.</p>
      </section>

      <section className="card">
        <h2>Download CSV</h2>
        <label className="field" htmlFor="export-unit" style={{ marginTop: 0 }}>
          Units
        </label>
        <select id="export-unit" value={exportUnit} onChange={(e) => setExportUnit(e.target.value)} style={{ maxWidth: 240 }}>
          <option value="">All units</option>
          {units.map((u) => (
            <option key={u} value={u}>
              Unit {u}
            </option>
          ))}
        </select>
        <ul className="plain">
          {EXPORTS.map((x) => (
            <li key={x.file}>
              <a href={`/api/export/${x.file}${q}`} download>
                {x.label}
              </a>{" "}
              <span className="muted">({x.file}: {x.hint})</span>
            </li>
          ))}
        </ul>
        <p className="muted">
          Times: <code>ts</code> is UTC (ISO 8601), <code>ts_local</code> is local time. Distances use the phones&apos;
          positions nearest in time (±2 min); for relayed frames (hops &gt; 0) it is the distance to the originator.
        </p>
      </section>
    </main>
  );
}
