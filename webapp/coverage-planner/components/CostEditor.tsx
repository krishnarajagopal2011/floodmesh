"use client";
/**
 * The admin's cost sheet editor. Component lines per unit kind, project
 * lines with their basis, markup and contingency. Saving replaces the whole
 * sheet on the server (PUT /api/admin/costs), which validates it again and
 * refuses it if someone else saved since this page loaded (revision check).
 */
import { useEffect, useRef, useState } from "react";
import {
  COST_BASES,
  formatInr,
  publicCosts,
  sumLines,
  validateCosts,
  type CostBasis,
  type CostLine,
  type CostSheet,
  type ProjectLine,
} from "@/lib/costs";
import { download } from "@/lib/exporters";
import { UNIT_LABELS, type UnitKind } from "@/lib/radio";
import { NumInput } from "./ui";

const KINDS: UnitKind[] = ["civilian", "powered", "responder"];
const NOTES: Record<UnitKind, string> = {
  civilian: "One per household that gets a unit. Indoors, on battery.",
  powered: "On a terrace with solar or UPS power, outdoor antenna; the price includes installation.",
  responder: "Carried by responders; placed on the map or added as extra handsets.",
};

let n = 0;
const lineId = () => `l${Date.now().toString(36)}${(n++).toString(36)}`;

function LinesTable<T extends CostLine>({
  lines,
  onChange,
  withBasis,
  markupPct,
}: {
  lines: T[];
  onChange: (l: T[]) => void;
  withBasis?: boolean;
  markupPct: number;
}) {
  const set = (i: number, patch: Partial<T>) => onChange(lines.map((l, j) => (j === i ? { ...l, ...patch } : l)));
  const move = (i: number, d: -1 | 1) => {
    const j = i + d;
    if (j < 0 || j >= lines.length) return;
    const next = [...lines];
    [next[i], next[j]] = [next[j], next[i]];
    onChange(next);
  };
  return (
    <div className="tableWrap">
      <table className="table editTable">
        <thead>
          <tr>
            <th>Item</th>
            {withBasis && <th>Basis</th>}
            <th className="num">Cost (₹)</th>
            {withBasis && markupPct > 0 && <th title="Add the markup to this line's public rate">Markup</th>}
            {markupPct > 0 && <th className="num">Price</th>}
            <th>Source / note</th>
            <th aria-label="Actions"></th>
          </tr>
        </thead>
        <tbody>
          {lines.map((l, i) => (
            <tr key={l.id}>
              <td>
                <input aria-label="Item" value={l.name} maxLength={120} onChange={(e) => set(i, { name: e.target.value } as Partial<T>)} />
              </td>
              {withBasis && (
                <td>
                  <select
                    aria-label="Basis"
                    value={(l as unknown as ProjectLine).basis}
                    onChange={(e) => set(i, { basis: e.target.value as CostBasis } as unknown as Partial<T>)}
                  >
                    {Object.entries(COST_BASES).map(([k, v]) => (
                      <option key={k} value={k}>
                        {v}
                      </option>
                    ))}
                  </select>
                </td>
              )}
              <td className="num">
                <NumInput ariaLabel="Cost in rupees" value={l.amount} min={0} max={1e9} step={1} onChange={(v) => set(i, { amount: v } as Partial<T>)} />
              </td>
              {withBasis && markupPct > 0 && (
                <td>
                  <input
                    type="checkbox"
                    aria-label="Add the markup to this line"
                    checked={(l as unknown as ProjectLine).markup}
                    onChange={(e) => set(i, { markup: e.target.checked } as unknown as Partial<T>)}
                  />
                </td>
              )}
              {markupPct > 0 && (
                <td className="num muted">
                  {formatInr(l.amount * (!withBasis || (l as unknown as ProjectLine).markup ? 1 + markupPct / 100 : 1))}
                </td>
              )}
              <td>
                <input aria-label="Note" value={l.note} maxLength={200} onChange={(e) => set(i, { note: e.target.value } as Partial<T>)} />
              </td>
              <td className="actions">
                <button type="button" className="mini" title="Move up" onClick={() => move(i, -1)} disabled={i === 0}>
                  ↑
                </button>
                <button type="button" className="mini" title="Move down" onClick={() => move(i, 1)} disabled={i === lines.length - 1}>
                  ↓
                </button>
                <button type="button" className="mini danger" title="Remove line" onClick={() => onChange(lines.filter((_, j) => j !== i))}>
                  ✕
                </button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <button
        type="button"
        className="secondary small"
        onClick={() =>
          onChange([
            ...lines,
            { id: lineId(), name: "New item", amount: 0, note: "", ...(withBasis ? { basis: "fixed", markup: true } : {}) } as unknown as T,
          ])
        }
        disabled={lines.length >= 40}
      >
        + Add line
      </button>
    </div>
  );
}

interface ServerReply {
  ok: boolean;
  error?: string;
  conflict?: boolean;
  sheet?: CostSheet;
  updatedAt?: string | null;
  isDefault?: boolean;
  rev?: number | null;
}

/** "Last saved" in India time, so the server render and the browser agree. */
function savedLabel(updatedAt: string | null): string {
  if (!updatedAt) return "";
  return new Date(updatedAt).toLocaleString("en-IN", { dateStyle: "medium", timeStyle: "short", timeZone: "Asia/Kolkata" });
}

export default function CostEditor({
  initial,
  initialUpdatedAt,
  initialIsDefault,
  initialRev,
}: {
  initial: CostSheet;
  initialUpdatedAt: string | null;
  initialIsDefault: boolean;
  initialRev: number | null;
}) {
  const [sheet, setSheet] = useState<CostSheet>(initial);
  const [saved, setSaved] = useState<CostSheet>(initial);
  const [updatedAt, setUpdatedAt] = useState(initialUpdatedAt);
  const [isDefault, setIsDefault] = useState(initialIsDefault);
  const [rev, setRev] = useState<number | null>(initialRev);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ tone: "ok" | "bad"; text: string } | null>(null);
  /** The newer sheet from the server after a 409, until the admin chooses what to do. */
  const [conflict, setConflict] = useState<ServerReply | null>(null);
  const fileRef = useRef<HTMLInputElement>(null);
  const dirty = JSON.stringify(sheet) !== JSON.stringify(saved);

  useEffect(() => {
    const warn = (e: BeforeUnloadEvent) => {
      if (dirty) e.preventDefault();
    };
    window.addEventListener("beforeunload", warn);
    return () => window.removeEventListener("beforeunload", warn);
  }, [dirty]);

  function adopt(j: ServerReply) {
    setSheet(j.sheet!);
    setSaved(j.sheet!);
    setUpdatedAt(j.updatedAt ?? null);
    setIsDefault(Boolean(j.isDefault));
    setRev(j.rev ?? null);
  }

  async function send(method: "PUT" | "DELETE", body?: CostSheet, force = false) {
    setBusy(true);
    setMsg(null);
    setConflict(null);
    try {
      const expected = rev === null ? "none" : String(rev);
      const res = await fetch(
        method === "PUT" ? "/api/admin/costs" : `/api/admin/costs?expectedRev=${expected}${force ? "&force=1" : ""}`,
        {
          method,
          headers: body ? { "Content-Type": "application/json" } : undefined,
          body: body ? JSON.stringify({ sheet: body, expectedRev: rev, force }) : undefined,
        },
      );
      let j: ServerReply;
      try {
        j = (await res.json()) as ServerReply;
      } catch {
        // A platform error page, not this API's JSON.
        setMsg({ tone: "bad", text: `Not saved: the server answered HTTP ${res.status}. Try again in a minute.` });
        return;
      }
      if (res.status === 401) {
        setMsg({
          tone: "bad",
          text: "Your login has expired. Your changes are still on this page: log in again at /admin in a new tab, then press Save here. Or use Download sheet to keep a copy.",
        });
        return;
      }
      if (res.status === 409 && j.sheet) {
        setConflict(j);
        return;
      }
      if (!j.ok || !j.sheet) {
        setMsg({ tone: "bad", text: j.error ?? `Not saved (HTTP ${res.status}).` });
        return;
      }
      adopt(j);
      setMsg({ tone: "ok", text: method === "PUT" ? "Saved. The planner uses the new prices from its next page load." : "Reset to the built-in defaults." });
    } catch {
      setMsg({ tone: "bad", text: "Not saved: no connection to the server." });
    } finally {
      setBusy(false);
    }
  }

  function save(force = false) {
    const v = validateCosts(sheet);
    if (!v.ok) {
      setMsg({ tone: "bad", text: v.error });
      return;
    }
    void send("PUT", v.sheet, force);
  }

  async function importSheet(file: File) {
    try {
      const v = validateCosts(JSON.parse(await file.text()));
      if (!v.ok) throw new Error(v.error);
      setSheet(v.sheet);
      setMsg({ tone: "ok", text: `Loaded ${file.name}. Check it, then Save.` });
    } catch (err) {
      setMsg({ tone: "bad", text: `Could not load ${file.name}: ${err instanceof Error ? err.message : "not a cost sheet"}` });
    }
  }

  const pub = publicCosts(sheet, updatedAt, isDefault);

  return (
    <main className="adminPage">
      <div className="adminTop">
        <div>
          <h1>Planner admin: costs</h1>
          <p className="muted" suppressHydrationWarning>
            {isDefault ? "Built-in placeholder prices are in use; nothing has been saved yet." : `Last saved ${savedLabel(updatedAt)} (India time).`}{" "}
            The planner shows one price per unit type and one rate per project line; this breakdown stays on this page.
          </p>
        </div>
        <div className="row">
          <a href="/">Planner</a>
          <form method="post" action="/api/logout">
            <button type="submit" className="secondary">
              Log out
            </button>
          </form>
        </div>
      </div>

      <div className="saveBar">
        <button type="button" onClick={() => save()} disabled={busy || !dirty}>
          {busy ? "Saving…" : dirty ? "Save changes" : "Saved"}
        </button>
        <button type="button" className="secondary" onClick={() => setSheet(saved)} disabled={busy || !dirty}>
          Discard changes
        </button>
        <span className="spacer" />
        <button type="button" className="secondary" onClick={() => download("floodmesh-cost-sheet.json", JSON.stringify(sheet, null, 1), "application/json")}>
          Download sheet
        </button>
        <button type="button" className="secondary" onClick={() => fileRef.current?.click()} disabled={busy}>
          Load sheet
        </button>
        <input
          ref={fileRef}
          type="file"
          accept=".json,application/json"
          hidden
          onChange={(e) => {
            const f = e.target.files?.[0];
            if (f) void importSheet(f);
            e.target.value = "";
          }}
        />
        <button
          type="button"
          className="secondary danger"
          disabled={busy}
          onClick={() => {
            if (window.confirm("Replace the saved price list with the built-in defaults?")) void send("DELETE");
          }}
        >
          Reset to defaults
        </button>
      </div>
      {msg && (
        <p className={msg.tone === "ok" ? "okBox" : "error"} role="status">
          {msg.text}
        </p>
      )}
      {conflict && (
        <div className="error" role="alert">
          <p>
            Not saved: {conflict.error} The newer list was{" "}
            {conflict.isDefault ? "reset to the built-in defaults" : `saved ${savedLabel(conflict.updatedAt ?? null)} (India time)`}.
          </p>
          <div className="row wrap">
            <button
              type="button"
              className="secondary"
              onClick={() => {
                if (!dirty || window.confirm("Load the newer list? Your unsaved changes on this page are lost.")) {
                  adopt(conflict);
                  setConflict(null);
                }
              }}
            >
              Load the newer list
            </button>
            <button
              type="button"
              className="danger"
              onClick={() => {
                if (window.confirm("Replace the newer list with the one on this page?")) save(true);
              }}
            >
              Overwrite it with mine
            </button>
          </div>
        </div>
      )}

      <fieldset className="plain" disabled={busy}>
      <section className="card">
        <h2>Pricing</h2>
        <div className="row wrap">
          <label className="inline">
            Markup on cost
            <NumInput value={sheet.markupPct} min={0} max={500} step={0.5} onChange={(v) => setSheet({ ...sheet, markupPct: v })} />%
          </label>
          <label className="inline">
            Contingency (shown in the planner)
            <NumInput value={sheet.contingencyPct} min={0} max={500} step={0.5} onChange={(v) => setSheet({ ...sheet, contingencyPct: v })} />%
          </label>
        </div>
        <table className="table compact priceSummary">
          <thead>
            <tr>
              <th>Unit</th>
              <th className="num">Cost</th>
              <th className="num">Price in the planner</th>
            </tr>
          </thead>
          <tbody>
            {KINDS.map((k) => (
              <tr key={k}>
                <td>{UNIT_LABELS[k]}</td>
                <td className="num">{formatInr(sumLines(sheet.units[k]))}</td>
                <td className="num">
                  <strong>{formatInr(pub.unitPrice[k])}</strong>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </section>

      {KINDS.map((k) => (
        <section className="card" key={k}>
          <div className="sectionHead">
            <h2>{UNIT_LABELS[k]}</h2>
            <strong>{formatInr(sumLines(sheet.units[k]))}</strong>
          </div>
          <p className="muted">{NOTES[k]}</p>
          <LinesTable
            lines={sheet.units[k]}
            markupPct={sheet.markupPct}
            onChange={(lines) => setSheet({ ...sheet, units: { ...sheet.units, [k]: lines } })}
          />
          {k !== "civilian" && (
            <button
              type="button"
              className="secondary small"
              onClick={() => {
                const ids = new Set(sheet.units[k].map((l) => l.id));
                const add = sheet.units.civilian.filter((l) => !ids.has(l.id)).map((l) => ({ ...l }));
                setSheet({ ...sheet, units: { ...sheet.units, [k]: [...add, ...sheet.units[k]] } });
              }}
            >
              Copy missing household-unit lines here
            </button>
          )}
        </section>
      ))}

      <section className="card">
        <h2>Project costs</h2>
        <p className="muted">Survey, testing, certification, training and maintenance. Each line is multiplied by its basis in the planner.</p>
        <LinesTable withBasis lines={sheet.project} markupPct={sheet.markupPct} onChange={(project) => setSheet({ ...sheet, project })} />
        {sheet.markupPct > 0 && (
          <p className="hint">
            The markup is added to every unit price and to the project lines ticked under Markup. Anyone who knows a line&apos;s real cost can
            work the markup out from its public rate, so leave public fees (such as WPC approval) unticked.
          </p>
        )}
      </section>
      </fieldset>

      <p className="hint">
        Default sources: electronics from the priced PCB V1 BoM (docs/hardware/pcb-v1, 15-unit build, landed), the whip antenna from
        docs/architecture.md §14.1, WPC type approval from docs/certification-india.md. Lines marked “Estimate” are placeholders: replace
        them with quotes.
      </p>
    </main>
  );
}
