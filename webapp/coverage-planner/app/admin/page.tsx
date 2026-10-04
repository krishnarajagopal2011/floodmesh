/**
 * Admin page: log in with the admin password, then edit the cost sheet the
 * planner uses. A plain HTML form for the login, so it works without
 * JavaScript; the cost editor itself is a client component.
 */
import CostEditor from "@/components/CostEditor";
import { loadCosts } from "@/lib/costStore";
import { getSession } from "@/lib/session";

export const dynamic = "force-dynamic";

type Search = Promise<Record<string, string | string[] | undefined>>;

/** The only texts ?error= can show; an unknown code shows nothing. */
const LOGIN_ERRORS: Record<string, string> = {
  origin: "Request from another site refused.",
  empty: "Enter the password.",
  wrong: "Wrong password.",
  setup: "The admin password is not set on the server (ADMIN_PASSWORD).",
  locked: "Too many wrong passwords.",
};

function errorText(code: string, wait: string): string | null {
  if (!Object.hasOwn(LOGIN_ERRORS, code)) return null;
  if (code !== "locked") return LOGIN_ERRORS[code];
  const mins = /^\d{1,2}$/.test(wait) ? Number(wait) : 0;
  return `${LOGIN_ERRORS.locked} Try again ${mins >= 1 && mins <= 15 ? `in ${mins} min` : "later"}.`;
}

export default async function AdminPage({ searchParams }: { searchParams: Search }) {
  const s = await getSession();
  if (s) {
    let c: Awaited<ReturnType<typeof loadCosts>>;
    try {
      c = await loadCosts();
    } catch (err) {
      console.error("[admin] could not load the cost sheet", err);
      return (
        <main className="adminPage narrow">
          <h1>Planner admin</h1>
          <p className="error" role="alert">
            The database is unavailable right now, so the price list cannot be shown. Reload in a minute.
          </p>
          <a href="/">Back to the planner</a>
        </main>
      );
    }
    return <CostEditor initial={c.sheet} initialUpdatedAt={c.updatedAt} initialIsDefault={c.isDefault} initialRev={c.rev} />;
  }
  const sp = await searchParams;
  const one = (v: string | string[] | undefined) => (Array.isArray(v) ? v[0] : v) ?? "";
  const error = errorText(one(sp.error), one(sp.wait));
  return (
    <main className="adminPage narrow">
      <div className="adminTop">
        <div>
          <h1>Planner admin</h1>
          <p className="muted">Unit prices and project costs used by the FloodMesh coverage planner.</p>
        </div>
        <a href="/">Back to the planner</a>
      </div>
      {error && (
        <p className="error" role="alert">
          {error}
        </p>
      )}
      <form className="card" method="post" action="/api/login">
        <label className="fieldLabel" htmlFor="pw">
          Admin password
        </label>
        <input id="pw" name="password" type="password" autoComplete="current-password" required autoFocus />
        <div className="row">
          <button type="submit">Log in</button>
        </div>
      </form>
    </main>
  );
}
