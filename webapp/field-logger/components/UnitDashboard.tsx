"use client";
/**
 * The volunteer's screen. Polls /api/status every 15 s (and at once when
 * the page comes back to the foreground), so a glance tells the volunteer
 * whether their unit is still uploading.
 */
import { useCallback, useEffect, useState } from "react";
import type { RecordView, UnitStatus } from "@/lib/queries";
import { age, clock, dayClock, fixed, isStale } from "./format";
import LocationShare, { forgetLocationConsent } from "./LocationShare";
import NotesBox from "./NotesBox";

const REFRESH_MS = 15_000;

function summary(r: RecordView): string {
  switch (r.k) {
    case "rx":
      return [r.text, r.ch !== null ? `ch ${r.ch}` : ""].filter(Boolean).join(" · ");
    case "echo":
      return `passed on${r.id ? ` (#${r.id})` : ""}`;
    case "tx":
      return [r.ok === false ? "NOT SENT" : "sent", r.text, r.ch !== null ? `ch ${r.ch}` : ""]
        .filter(Boolean)
        .join(" · ");
    case "relay":
      return `relayed${r.id ? ` #${r.id}` : ""}`;
    case "sos":
      return [r.state, r.ch !== null ? `ch ${r.ch}` : "", r.n !== null ? `try ${r.n}` : ""].filter(Boolean).join(" · ");
    case "status":
      return [
        r.batt_v !== null ? `${fixed(r.batt_v, 2)} V` : "",
        r.batt_pct !== null ? `${r.batt_pct}%` : "",
        r.powered ? "powered" : "",
      ]
        .filter(Boolean)
        .join(" · ");
    case "boot":
      return [r.reason, r.text].filter(Boolean).join(" · ");
    default:
      return r.text ?? "";
  }
}

export default function UnitDashboard({
  unit,
  loginId,
  isAdmin,
  units,
}: {
  unit: string;
  loginId: string;
  isAdmin: boolean;
  units: string[];
}) {
  const [data, setData] = useState<UnitStatus | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loggedOut, setLoggedOut] = useState(false);
  const [fetchedAt, setFetchedAt] = useState<number | null>(null);
  const [now, setNow] = useState(() => Date.now());

  const load = useCallback(async () => {
    try {
      const res = await fetch(`/api/status?unit=${encodeURIComponent(unit)}`, { cache: "no-store" });
      if (res.status === 401) {
        setLoggedOut(true);
        return;
      }
      const body = (await res.json()) as { ok: boolean; error?: string } & UnitStatus;
      if (!res.ok || !body.ok) throw new Error(body.error ?? `HTTP ${res.status}`);
      setData(body);
      setError(null);
      setFetchedAt(Date.now());
    } catch (err) {
      setError(`Could not refresh: ${(err as Error).message}. Retrying.`);
    }
  }, [unit]);

  useEffect(() => {
    load();
    const poll = setInterval(load, REFRESH_MS);
    const tick = setInterval(() => setNow(Date.now()), 5_000);
    const onVisible = () => {
      if (document.visibilityState === "visible") load();
    };
    document.addEventListener("visibilitychange", onVisible);
    return () => {
      clearInterval(poll);
      clearInterval(tick);
      document.removeEventListener("visibilitychange", onVisible);
    };
  }, [load]);

  const up = data?.upload ?? null;
  const stale = isStale(up?.received_at, now);
  const batt = data?.battery ?? null;

  return (
    <main>
      <div className="top">
        <div>
          <h1>Unit {unit}</h1>
          <small>FloodMesh field test{isAdmin ? " · admin view" : ""}</small>
        </div>
        <div className="row" style={{ marginTop: 0 }}>
          {isAdmin && (
            <a className="button secondary" href="/admin">
              All units
            </a>
          )}
          {/* The next person to log in on this phone must see the location consent again. */}
          <form method="post" action="/api/logout" onSubmit={forgetLocationConsent}>
            <button type="submit" className="secondary">
              Log out
            </button>
          </form>
        </div>
      </div>

      {isAdmin && (
        <div className="row" aria-label="Choose unit">
          {units.map((u) => (
            <a key={u} className={`button${u === unit ? "" : " secondary"}`} href={`/unit?u=${encodeURIComponent(u)}`}>
              {u}
            </a>
          ))}
        </div>
      )}

      {loggedOut && (
        <p className="error" role="alert">
          Your login has expired. <a href="/login">Log in again</a>.
        </p>
      )}
      {error && !loggedOut && (
        <p className="error" role="status">
          {error}
        </p>
      )}

      {data && data.conflicts > 0 && (
        <section className="card warn" role="alert">
          <p>
            <strong>
              {data.conflicts} record{data.conflicts === 1 ? "" : "s"} from this unit reused numbers already stored.
            </strong>{" "}
            That happens when the device is swapped or its memory erased. The records are kept, but the list below
            may not show them. Tell the test lead.
          </p>
        </section>
      )}

      <section className={`card ${data ? (stale ? "bad" : "ok") : ""}`} aria-live="polite">
        <div className="label">Last upload from the unit</div>
        {!data ? (
          <div className="big">Loading…</div>
        ) : up ? (
          <>
            <div className="big">
              {dayClock(up.received_at)}{" "}
              <span className={stale ? "badText" : "okText"}>({age(up.received_at, now)})</span>
            </div>
            {stale && (
              <p className="badText">
                No upload for over 10 minutes. Check the unit is on and the phone hotspot is running.
              </p>
            )}
            <p className="muted">
              fw {up.fw ?? "?"} · image {up.image ?? "?"} · boot {up.boot ?? "?"}
              {up.dropped ? <span className="badText"> · {up.dropped} records lost (log full)</span> : null}
            </p>
          </>
        ) : (
          <>
            <div className="big badText">Nothing received yet</div>
            <p>Switch the unit on and turn on the phone hotspot. The first upload comes about a minute after boot.</p>
          </>
        )}
      </section>

      {data && (
        <div className="grid2">
          <section className="card">
            <div className="label">Battery</div>
            {batt ? (
              <>
                <div className="big">
                  {batt.batt_v !== null ? `${fixed(batt.batt_v, 2)} V` : "?"}
                  {batt.batt_pct !== null ? ` · ${batt.batt_pct}%` : ""}
                </div>
                <p className="muted">
                  {batt.powered ? "On external power · " : ""}reported {dayClock(batt.at)}
                  {batt.heard !== null ? ` · hears ${batt.heard} stations` : ""}
                </p>
              </>
            ) : (
              <div className="big muted">No status yet</div>
            )}
          </section>
          <section className="card">
            <div className="label">Records stored</div>
            <div className="big">{data.totals.today} today</div>
            <p className="muted">{data.totals.all} in total</p>
          </section>
        </div>
      )}

      <LocationShare unit={unit} loginId={loginId} isAdmin={isAdmin} lastStored={data?.location ?? null} now={now} />

      <NotesBox unit={unit} isAdmin={isAdmin} notes={data?.notes ?? []} onSaved={load} />

      <section className="card">
        <h2>Latest records</h2>
        <p className="muted">
          Newest first, in your phone&apos;s time zone. — means the time is unknown (made before a restart, before
          the unit&apos;s clock was set).
        </p>
        <div className="tableWrap">
          <table>
            <thead>
              <tr>
                <th>Time</th>
                <th>Kind</th>
                <th>From</th>
                <th className="num">Hops</th>
                <th className="num">dBm</th>
                <th className="num">SNR</th>
                <th>Text</th>
              </tr>
            </thead>
            <tbody>
              {(data?.records ?? []).map((r) => (
                <tr key={r.seq} className={`k-${r.k}${r.t ? ` t-${r.t}` : ""}`}>
                  <td title={r.ts ? `time from ${r.ts_src === "unit" ? "unit clock" : "uptime"}` : "time unknown"}>
                    {dayClock(r.ts)}
                  </td>
                  <td>
                    <strong>{r.k}</strong>
                    {r.t ? ` ${r.t}` : ""}
                  </td>
                  <td>{r.from ?? ""}</td>
                  <td className="num">{r.hops ?? ""}</td>
                  <td className="num">{r.rssi !== null ? Math.round(r.rssi) : ""}</td>
                  <td className="num">{r.snr !== null ? Math.round(r.snr * 100) / 100 : ""}</td>
                  <td className="wrap">{summary(r)}</td>
                </tr>
              ))}
              {data && data.records.length === 0 && (
                <tr>
                  <td colSpan={7} className="muted">
                    No records yet.
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      </section>

      <p className="muted">
        {fetchedAt ? `Updated ${clock(new Date(fetchedAt).toISOString())}` : "Loading"} · refreshes every 15 s
      </p>
    </main>
  );
}
