/**
 * Login: pick the unit you were handed, type its passcode. The test lead
 * logs in as admin below. Plain HTML forms, so logging in works even when
 * the page's scripts did not load on a weak mobile connection.
 */
import { units } from "@/lib/config";
import { getSession } from "@/lib/session";

export const dynamic = "force-dynamic";

type Search = Promise<Record<string, string | string[] | undefined>>;

/**
 * The only texts ?error= can show. The login route redirects with a code;
 * an unknown code shows nothing, so a link cannot put its own words in the
 * red alert box on the real site.
 */
const LOGIN_ERRORS: Record<string, string> = {
  origin: "Request from another site refused.",
  unit: "Choose your unit.",
  passcode: "Enter the passcode.",
  wrong: "Wrong passcode.",
  setup: "This login is not set up yet. Ask the test lead.",
  locked: "Too many wrong passcodes.",
};

function errorText(code: string, wait: string): string | null {
  if (!Object.hasOwn(LOGIN_ERRORS, code)) return null;
  if (code !== "locked") return LOGIN_ERRORS[code];
  // The wait is only a number, and only within the 15-minute window.
  const mins = /^\d{1,2}$/.test(wait) ? Number(wait) : 0;
  return `${LOGIN_ERRORS.locked} Try again ${mins >= 1 && mins <= 15 ? `in ${mins} min` : "later"}.`;
}

export default async function LoginPage({ searchParams }: { searchParams: Search }) {
  const sp = await searchParams;
  const one = (v: string | string[] | undefined) => (Array.isArray(v) ? v[0] : v) ?? "";
  const error = errorText(one(sp.error), one(sp.wait));
  const role = one(sp.role);
  const picked = one(sp.unit);
  const list = units();
  const s = await getSession();

  return (
    <main style={{ maxWidth: 520 }}>
      <div className="top">
        <div>
          <h1>FloodMesh field test</h1>
          <small>Unit logger</small>
        </div>
      </div>

      {s && (
        <div className="card ok">
          <p>
            Logged in as <strong>{s.role === "admin" ? "admin" : `Unit ${s.unit}`}</strong>.
          </p>
          <div className="row">
            <a className="button" href={s.role === "admin" ? "/admin" : "/unit"}>
              Continue
            </a>
          </div>
        </div>
      )}

      {error && (
        <p className="error" role="alert">
          {error}
        </p>
      )}

      <form className="card" method="post" action="/api/login">
        <input type="hidden" name="role" value="unit" />
        <h2>Volunteer</h2>
        <p className="muted">Choose the unit letter written on the device you were given.</p>
        <fieldset style={{ border: 0, padding: 0, margin: "10px 0 0" }}>
          <legend className="label">Unit</legend>
          <div className="units">
            {list.map((u) => (
              <label key={u}>
                <input type="radio" name="unit" value={u} required defaultChecked={picked === u} />
                <span>{u}</span>
              </label>
            ))}
          </div>
        </fieldset>
        <label className="field" htmlFor="unit-pass">
          Passcode
        </label>
        <input
          id="unit-pass"
          name="passcode"
          type="password"
          autoComplete="current-password"
          autoCapitalize="none"
          autoCorrect="off"
          spellCheck={false}
          required
        />
        <div className="row">
          <button type="submit" style={{ width: "100%" }}>
            Log in
          </button>
        </div>
      </form>

      <details className="card" open={role === "admin"}>
        <summary>Test lead (admin)</summary>
        <form method="post" action="/api/login">
          <input type="hidden" name="role" value="admin" />
          <label className="field" htmlFor="admin-pass">
            Admin passcode
          </label>
          <input id="admin-pass" name="passcode" type="password" autoComplete="current-password" required />
          <div className="row">
            <button type="submit" className="secondary">
              Log in as admin
            </button>
          </div>
        </form>
      </details>
    </main>
  );
}
