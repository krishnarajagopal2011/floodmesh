/**
 * POST /api/login: admin login.
 *
 * Accepts the admin page's plain HTML form (works without JavaScript) and
 * JSON (curl). A form post is answered with a redirect, JSON with JSON. A
 * failed form login redirects to /admin?error=<code>; the page shows only its
 * own fixed text for a known code.
 */
import { adminPassword } from "@/lib/config";
import { safeEqual } from "@/lib/crypto";
import { clientIp, json, readBodyLimited, sameOrigin, secureCookies } from "@/lib/http";
import { beginLoginAttempt, loginSucceeded, recordLoginFailure } from "@/lib/ratelimit";
import { SESSION_COOKIE, SESSION_TTL_S, cookieOptions, signSession } from "@/lib/session";
import { NextResponse } from "next/server";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

function seeOther(location: string): NextResponse {
  return new NextResponse(null, { status: 303, headers: { Location: location, "Cache-Control": "no-store" } });
}

export async function POST(req: Request): Promise<Response> {
  const isForm = (req.headers.get("content-type") ?? "").includes("application/x-www-form-urlencoded");
  const raw = await readBodyLimited(req, 2048);
  let password = "";
  try {
    if (raw) {
      const text = raw.toString("utf8");
      const v = isForm ? new URLSearchParams(text).get("password") : (JSON.parse(text) as { password?: unknown }).password;
      password = typeof v === "string" ? v : "";
    }
  } catch {
    password = "";
  }

  const fail = (status: number, code: string, message: string, headers: Record<string, string> = {}, wait?: number) => {
    if (!isForm) return json({ ok: false, error: message }, status, headers);
    const q = new URLSearchParams({ error: code, ...(wait ? { wait: String(wait) } : {}) });
    return seeOther(`/admin?${q}`);
  };

  if (!sameOrigin(req)) return fail(403, "origin", "Request from another site refused.");
  if (!password) return fail(400, "empty", "Enter the password.");

  const ip = clientIp(req);
  const wait = await beginLoginAttempt(ip, "admin");
  if (wait > 0) {
    const mins = Math.ceil(wait / 60);
    console.warn(`[login] refused from ${ip}: rate limit, ${mins} min left`);
    return fail(429, "locked", `Too many wrong passwords. Try again in ${mins} min.`, { "Retry-After": String(wait) }, mins);
  }

  const expected = adminPassword();
  if (!expected || !safeEqual(password, expected)) {
    await recordLoginFailure("admin");
    await new Promise((r) => setTimeout(r, 400));
    console.warn(`[login] failed from ${ip}`);
    return expected
      ? fail(401, "wrong", "Wrong password.")
      : fail(401, "setup", "ADMIN_PASSWORD is not set on the server.");
  }
  await loginSucceeded(ip);

  const exp = Math.floor(Date.now() / 1000) + SESSION_TTL_S;
  const res = isForm ? seeOther("/admin") : NextResponse.json({ ok: true, redirect: "/admin" });
  res.cookies.set(SESSION_COOKIE, signSession({ role: "admin", exp }), cookieOptions(secureCookies(req)));
  res.headers.set("Cache-Control", "no-store");
  console.log(`[login] admin from ${ip}`);
  return res;
}
