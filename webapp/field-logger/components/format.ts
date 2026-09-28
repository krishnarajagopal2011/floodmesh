/** Display helpers shared by the client components (phone local time). */

/** A unit is "late" once its last upload is older than two upload periods. */
export const STALE_MS = 10 * 60_000;

export function clock(isoTs: string | null | undefined): string {
  if (!isoTs) return "—";
  return new Date(isoTs).toLocaleTimeString("en-GB", { hour12: false });
}

export function dayClock(isoTs: string | null | undefined): string {
  if (!isoTs) return "—";
  const d = new Date(isoTs);
  const today = new Date();
  const sameDay = d.toDateString() === today.toDateString();
  const t = d.toLocaleTimeString("en-GB", { hour12: false });
  return sameDay ? t : `${d.toLocaleDateString("en-GB", { day: "2-digit", month: "short" })} ${t}`;
}

export function age(isoTs: string | null | undefined, now: number): string {
  if (!isoTs) return "never";
  const s = Math.max(0, Math.round((now - Date.parse(isoTs)) / 1000));
  if (s < 60) return `${s} s ago`;
  const m = Math.floor(s / 60);
  if (m < 90) return `${m} min ago`;
  const h = Math.floor(m / 60);
  if (h < 48) return `${h} h ${m % 60} min ago`;
  return `${Math.floor(h / 24)} days ago`;
}

export function isStale(isoTs: string | null | undefined, now: number): boolean {
  return !isoTs || now - Date.parse(isoTs) > STALE_MS;
}

export function fixed(v: number | null | undefined, digits: number): string {
  return v === null || v === undefined ? "" : v.toFixed(digits);
}
