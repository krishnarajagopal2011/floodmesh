/** POST /api/logout: clear the session cookie and go back to the planner. */
import { sameOrigin, secureCookies } from "@/lib/http";
import { SESSION_COOKIE, cookieOptions } from "@/lib/session";
import { NextResponse } from "next/server";

export const runtime = "nodejs";

export async function POST(req: Request): Promise<Response> {
  if (!sameOrigin(req)) return new Response("cross-site request refused", { status: 403 });
  const res = new NextResponse(null, { status: 303, headers: { Location: "/", "Cache-Control": "no-store" } });
  res.cookies.set(SESSION_COOKIE, "", cookieOptions(secureCookies(req), 0));
  return res;
}
