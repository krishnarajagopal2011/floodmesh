/** Admin page: every unit at a glance, phone positions on a map, CSV exports. */
import { redirect } from "next/navigation";
import { units } from "@/lib/config";
import { getSession } from "@/lib/session";
import AdminDashboard from "@/components/AdminDashboard";

export const dynamic = "force-dynamic";

export default async function AdminPage() {
  const s = await getSession();
  if (!s) redirect("/login?role=admin");
  if (s.role !== "admin") redirect("/unit");
  return <AdminDashboard units={units()} />;
}
