/**
 * POST /api/login: unit or admin login.
 *
 * Accepts the login page's plain HTML form (so it works even if the page's
 * JavaScript failed to load on a weak connection) and JSON (curl, simulator).
 * Form posts are answered with a redirect, JSON with JSON.
 *
 * A failed form login redirects to /login?error=<code>, never to the message
 * itself: the login page shows only its own fixed text for a known code, so
 * a crafted link cannot make the real site display words of someone else's
 * choosing as an official error.
 */
import { adminPasscode, isUnit, unitPasscode } from "@/lib/config";
import { safeEqual } from "@/lib/crypto";
import { clientIp, json, readBodyLimited, sameOrigin, secureCookies } from "@/lib/http";
import { beginLoginAttempt, loginSucceeded, recordLoginFailure } from "@/lib/ratelimit";
import { SESSION_COOKIE, SESSION_TTL_S, cookieOptions, signSession, type Session } from "@/lib/session";
import { NextResponse } from "next/server";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

/**
 * 303 with a relative Location: correct behind Vercel's proxy and on a LAN
 * address alike, where an absolute URL built from req.url might not be.
 */
function seeOther(location: string): NextResponse {
  return new NextResponse(null, { status: 303, headers: { Location: location, "Cache-Control": "no-store" } });
}

export async function POST(req: Request): Promise<Response> {
  const isForm = (req.headers.get("content-type") ?? "").includes("application/x-www-form-urlencoded");
  const raw = await readBodyLimited(req, 2048);
  let fields: Record<string, string> = {};
  try {
    if (raw) {
      const text = raw.toString("utf8");
      fields = isForm
        ? Object.fromEntries(new URLSearchParams(text))
        : Object.fromEntries(
            Object.entries(JSON.parse(text) as Record<string, unknown>).map(([k, v]) => [k, String(v ?? "")]),
          );
    }
  } catch {
    fields = {};
  }

  const role = fields.role === "admin" ? "admin" : "unit";
  const unit = (fields.unit ?? "").trim();
  const passcode = fields.passcode ?? "";
  const account = role === "admin" ? "admin" : unit;

  /** `code` is one of LOGIN_ERRORS' keys (app/login/page.tsx), `message` its text for JSON callers. */
  const fail = (
    status: number,
    code: string,
    message: string,
    headers: Record<string, string> = {},
    extra: Record<string, string> = {},
  ) => {
    if (!isForm) return json({ ok: false, error: message }, status, headers);
    const q = new URLSearchParams({ error: code, ...extra, ...(role === "unit" && unit ? { unit } : {}), role });
    return seeOther(`/login?${q}`);
  };

  if (!sameOrigin(req)) return fail(403, "origin", "Request from another site refused.");
  if (role === "unit" && !isUnit(unit)) return fail(400, "unit", "Choose your unit.");
  if (!passcode) return fail(400, "passcode", "Enter the passcode.");

  // Counted before the compare (lib/ratelimit.ts), so parallel guesses
  // cannot all slip in under the limit.
  const ip = clientIp(req);
  const wait = await beginLoginAttempt(ip, account);
  if (wait > 0) {
    const mins = Math.ceil(wait / 60);
    console.warn(`[login] refused for ${account} from ${ip}: rate limit, ${mins} min left`);
    return fail(
      429,
      "locked",
      `Too many wrong passcodes. Try again in ${mins} min.`,
      { "Retry-After": String(wait) },
      { wait: String(mins) },
    );
  }

  const expected = role === "admin" ? adminPasscode() : unitPasscode(unit);
  if (!expected || !safeEqual(passcode, expected)) {
    await recordLoginFailure(account);
    // A fixed pause makes guessing slower still without tying up much.
    await new Promise((r) => setTimeout(r, 400));
    console.warn(`[login] failed for ${account} from ${ip}`);
    return expected
      ? fail(401, "wrong", "Wrong passcode.")
      : fail(401, "setup", "This login is not set up yet. Ask the test lead.");
  }
  await loginSucceeded(ip);

  const exp = Math.floor(Date.now() / 1000) + SESSION_TTL_S;
  const session: Session = role === "admin" ? { role: "admin", exp } : { role: "unit", unit, exp };
  const target = role === "admin" ? "/admin" : "/unit";
  const res = isForm
    ? seeOther(target)
    : NextResponse.json({ ok: true, role, unit: role === "unit" ? unit : null, redirect: target });
  res.cookies.set(SESSION_COOKIE, signSession(session), cookieOptions(secureCookies(req)));
  res.headers.set("Cache-Control", "no-store");
  console.log(`[login] ${account} from ${ip}`);
  return res;
}
