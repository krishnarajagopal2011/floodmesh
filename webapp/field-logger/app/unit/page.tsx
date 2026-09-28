/**
 * Volunteer page: one unit's latest uploads, battery and records, plus
 * location sharing and notes. A unit login always sees its own unit; the
 * admin can look at any unit with ?u=B.
 */
import { redirect } from "next/navigation";
import { units } from "@/lib/config";
import { getSession } from "@/lib/session";
import UnitDashboard from "@/components/UnitDashboard";

export const dynamic = "force-dynamic";

type Search = Promise<Record<string, string | string[] | undefined>>;

export default async function UnitPage({ searchParams }: { searchParams: Search }) {
  const s = await getSession();
  if (!s) redirect("/login");
  const list = units();
  let unit: string;
  if (s.role === "unit") {
    unit = s.unit;
  } else {
    const sp = await searchParams;
    const asked = Array.isArray(sp.u) ? sp.u[0] : sp.u;
    unit = asked && list.includes(asked) ? asked : list[0];
  }
  // The session's expiry is unique per login; LocationShare ties the
  // volunteer's location consent to it.
  return <UnitDashboard unit={unit} loginId={String(s.exp)} isAdmin={s.role === "admin"} units={list} />;
}
