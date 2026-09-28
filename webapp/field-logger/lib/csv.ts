/**
 * CSV output for the post-test analysis.
 *
 * Text cells that a spreadsheet would treat as a formula (leading = + - @,
 * tab or carriage return) get a leading apostrophe: volunteers' notes and
 * units' texts are typed by people and must never execute in Excel or
 * Sheets. Numeric cells are written as numbers and left alone, so an RSSI of
 * -97 stays a number rather than becoming the text '-97.
 */
import { timeZone } from "./config";

export function csvCell(v: unknown): string {
  if (v === null || v === undefined) return "";
  if (typeof v === "number") return Number.isFinite(v) ? String(v) : "";
  if (typeof v === "boolean") return v ? "true" : "false";
  let s = typeof v === "object" ? JSON.stringify(v) : String(v);
  if (/^[\t\r]|^\s*[=+\-@]/.test(s)) s = `'${s}`;
  return /[",\r\n]/.test(s) ? `"${s.replaceAll('"', '""')}"` : s;
}

export function csvLine(values: unknown[]): string {
  return values.map(csvCell).join(",") + "\r\n";
}

let fmt: Intl.DateTimeFormat | null = null;

/** "2026-09-28 15:30:05" in the test's time zone, which Excel reads as a date. */
export function localTime(isoTs: string | null): string | null {
  if (!isoTs) return null;
  fmt ??= new Intl.DateTimeFormat("sv-SE", {
    timeZone: timeZone(),
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  });
  return fmt.format(new Date(isoTs));
}

/**
 * Stream a CSV built page by page, so a whole test's records never sit in
 * memory at once and the response is not held to Vercel's buffered-response
 * size limit.
 */
export function csvStream(header: string[], rows: AsyncIterable<unknown[]>, tag: string): ReadableStream<Uint8Array> {
  const enc = new TextEncoder();
  const it = rows[Symbol.asyncIterator]();
  let started = false;
  return new ReadableStream<Uint8Array>({
    async pull(controller) {
      try {
        if (!started) {
          started = true;
          controller.enqueue(enc.encode(csvLine(header)));
          return;
        }
        // Batch lines into one chunk per pull to keep the overhead low.
        let chunk = "";
        for (let i = 0; i < 500; i++) {
          const { done, value } = await it.next();
          if (done) {
            if (chunk) controller.enqueue(enc.encode(chunk));
            controller.close();
            return;
          }
          chunk += csvLine(value);
        }
        controller.enqueue(enc.encode(chunk));
      } catch (err) {
        console.error(`[export] ${tag}: ${(err as Error).message}`);
        controller.error(err);
      }
    },
  });
}
