"use client";
/** Small form and layout pieces shared by the panels. */
import { useEffect, useId, useState, type ReactNode } from "react";

export function Section({ title, children, aside }: { title: string; children: ReactNode; aside?: ReactNode }) {
  return (
    <section className="section">
      <div className="sectionHead">
        <h3>{title}</h3>
        {aside}
      </div>
      {children}
    </section>
  );
}

/**
 * A number input that lets the user type freely ("", "-", "1.") and only
 * hands back a value once it parses, clamped to [min, max].
 */
export function NumInput({
  value,
  onChange,
  min = -Infinity,
  max = Infinity,
  step = 1,
  id,
  disabled,
  ariaLabel,
}: {
  value: number;
  onChange: (v: number) => void;
  min?: number;
  max?: number;
  step?: number;
  id?: string;
  disabled?: boolean;
  ariaLabel?: string;
}) {
  const [text, setText] = useState(String(value));
  const [focused, setFocused] = useState(false);
  useEffect(() => {
    if (!focused) setText(String(value));
  }, [value, focused]);
  return (
    <input
      id={id}
      type="number"
      inputMode="decimal"
      aria-label={ariaLabel}
      value={text}
      step={step}
      min={Number.isFinite(min) ? min : undefined}
      max={Number.isFinite(max) ? max : undefined}
      disabled={disabled}
      onFocus={() => setFocused(true)}
      onBlur={() => {
        setFocused(false);
        setText(String(value));
      }}
      onChange={(e) => {
        setText(e.target.value);
        const v = Number(e.target.value);
        if (e.target.value.trim() !== "" && Number.isFinite(v)) onChange(Math.min(Math.max(v, min), max));
      }}
    />
  );
}

export function NumberField({
  label,
  value,
  onChange,
  min = -Infinity,
  max = Infinity,
  step = 1,
  unit,
  hint,
  disabled,
}: {
  label: string;
  value: number;
  onChange: (v: number) => void;
  min?: number;
  max?: number;
  step?: number;
  unit?: string;
  hint?: ReactNode;
  disabled?: boolean;
}) {
  const id = useId();
  return (
    <div className="field">
      <label htmlFor={id}>{label}</label>
      <div className="inputUnit">
        <NumInput id={id} value={value} onChange={onChange} min={min} max={max} step={step} disabled={disabled} />
        {unit && <span className="unit">{unit}</span>}
      </div>
      {hint && <div className="hint">{hint}</div>}
    </div>
  );
}

export function Stat({ label, value, tone, sub }: { label: string; value: ReactNode; tone?: "ok" | "warn" | "bad"; sub?: ReactNode }) {
  return (
    <div className={`stat ${tone ?? ""}`}>
      <div className="statLabel">{label}</div>
      <div className="statValue">{value}</div>
      {sub && <div className="statSub">{sub}</div>}
    </div>
  );
}

export function Check({ label, checked, onChange }: { label: string; checked: boolean; onChange: (v: boolean) => void }) {
  return (
    <label className="check">
      <input type="checkbox" checked={checked} onChange={(e) => onChange(e.target.checked)} />
      <span>{label}</span>
    </label>
  );
}

export function fmtM(m: number): string {
  if (!Number.isFinite(m)) return "–";
  return m >= 1000 ? `${(m / 1000).toFixed(m >= 10_000 ? 0 : 1)} km` : `${Math.round(m)} m`;
}

export function fmtPct(p: number): string {
  return `${p >= 99.95 ? "100" : p.toFixed(1)}%`;
}

export function fmtInt(n: number): string {
  return Math.round(n).toLocaleString("en-IN");
}
