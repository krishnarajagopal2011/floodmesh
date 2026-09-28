"use client";
/**
 * Free-text notes for the analysis: things the unit cannot record itself
 * ("moved to the terrace", "unit fell in water", "hotspot off 10 min").
 * Stored with the server's time against this unit.
 */
import { useState } from "react";
import type { NoteView } from "@/lib/queries";
import { dayClock } from "./format";

const MAX_NOTE = 1000;

export default function NotesBox({
  unit,
  isAdmin,
  notes,
  onSaved,
}: {
  unit: string;
  isAdmin: boolean;
  notes: NoteView[];
  onSaved: () => void;
}) {
  const [text, setText] = useState("");
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ ok: boolean; text: string } | null>(null);

  async function save(e: React.FormEvent) {
    e.preventDefault();
    const t = text.trim();
    if (!t) return;
    setBusy(true);
    setMsg(null);
    try {
      const res = await fetch("/api/note", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ text: t, unit }),
      });
      const body = (await res.json().catch(() => ({}))) as { ok?: boolean; error?: string };
      if (!res.ok || !body.ok) throw new Error(body.error ?? `HTTP ${res.status}`);
      setText("");
      setMsg({ ok: true, text: "Note saved." });
      onSaved();
    } catch (err) {
      // The text stays in the box so nothing typed is lost.
      setMsg({ ok: false, text: `Not saved: ${(err as Error).message}. Try again.` });
    } finally {
      setBusy(false);
    }
  }

  return (
    <section className="card">
      <h2>Notes</h2>
      <form onSubmit={save}>
        <label className="field" htmlFor="note" style={{ marginTop: 0 }}>
          What happened? {isAdmin ? `(saved for unit ${unit})` : ""}
        </label>
        <textarea
          id="note"
          value={text}
          maxLength={MAX_NOTE}
          onChange={(e) => setText(e.target.value)}
          placeholder="e.g. moved to the terrace, unit got wet, hotspot was off"
        />
        <div className="row">
          <button type="submit" disabled={busy || !text.trim()}>
            {busy ? "Saving…" : "Save note"}
          </button>
          {msg && <span className={msg.ok ? "okText" : "badText"}>{msg.text}</span>}
        </div>
      </form>
      {notes.length > 0 && (
        <ul className="plain">
          {notes.map((n, i) => (
            <li key={`${n.ts}-${i}`}>
              <strong>{dayClock(n.ts)}</strong>
              {n.author === "admin" ? " (admin)" : ""}: {n.text}
            </li>
          ))}
        </ul>
      )}
    </section>
  );
}
