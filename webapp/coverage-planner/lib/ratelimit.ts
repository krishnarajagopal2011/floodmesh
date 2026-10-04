/**
 * Login rate limiting, copied from the field logger (webapp/field-logger),
 * kept in the database so it holds across serverless instances. Guessing the
 * admin password has to be slow.
 *
 * Every attempt is counted BEFORE its passcode is compared, by one upsert on
 * a per-key counter row. Postgres runs concurrent upserts on one key one
 * after another, so each attempt sees the count including every attempt
 * before it. (Counting failures first and recording them after the compare
 * let a burst of 80 parallel guesses all pass a limit of 10.) A correct
 * passcode gives its count back, so only wrong ones use up the allowance.
 *
 *   Per client IP: at most MAX_PER_IP wrong passcodes per 15-minute window,
 *   then refused until the window ends.
 *   Per account: past MAX_PER_ACCOUNT wrong passcodes in the window, only a
 *   caller whose IP has no attempts of its own in the window may still try.
 *   A burst of guesses from elsewhere therefore cannot lock the admin out
 *   (their own IP is clean), while a guesser spread over many IPs gets one
 *   try per fresh IP.
 *
 * Windows are fixed (they start at the first attempt), which is simpler to
 * make atomic than a sliding window and just as slow for a guesser.
 */
import { query } from "./db";

const WINDOW_MIN = 15;
const MAX_PER_IP = 10;
const MAX_PER_ACCOUNT = 30;

const ipKey = (ip: string) => `ip:${ip}`;
const accountKey = (account: string) => `acct:${account}`;

/** Count one attempt for `$1` and return the count including it. */
const BUMP = `INSERT INTO login_limits AS l (key, window_start, n) VALUES ($1, now(), 1)
  ON CONFLICT (key) DO UPDATE SET
    window_start = CASE WHEN l.window_start <= now() - make_interval(mins => $2) THEN now() ELSE l.window_start END,
    n            = CASE WHEN l.window_start <= now() - make_interval(mins => $2) THEN 1 ELSE l.n + 1 END
  RETURNING n, window_start`;

function secondsLeft(windowStart: unknown): number {
  const start = new Date(windowStart as string | Date).getTime();
  return Math.max(1, Math.ceil((start + WINDOW_MIN * 60_000 - Date.now()) / 1000));
}

/**
 * Count this attempt against the caller's IP and check both limits.
 * Returns 0 when the passcode may be compared, otherwise the seconds to wait.
 */
export async function beginLoginAttempt(ip: string, account: string): Promise<number> {
  const rows = await query<{ ip_n: number; ip_start: unknown; acct_n: number | null; acct_start: unknown }>(
    `WITH ip AS (${BUMP})
     SELECT ip.n AS ip_n, ip.window_start AS ip_start, a.n AS acct_n, a.window_start AS acct_start
       FROM ip LEFT JOIN login_limits a
         ON a.key = $3 AND a.window_start > now() - make_interval(mins => $2)`,
    [ipKey(ip), WINDOW_MIN, accountKey(account)],
  );
  const r = rows[0];
  if (!r) return 0;
  const ipN = Number(r.ip_n);
  if (ipN > MAX_PER_IP) return secondsLeft(r.ip_start);
  // ipN counts this attempt, so > 1 means this IP already tried in the window.
  if (Number(r.acct_n ?? 0) >= MAX_PER_ACCOUNT && ipN > 1) return secondsLeft(r.acct_start);
  return 0;
}

/** A wrong passcode: the IP's attempt stays counted; count it against the account too. */
export async function recordLoginFailure(account: string): Promise<void> {
  await query(BUMP, [accountKey(account), WINDOW_MIN]);
  // Housekeeping on the (rare) failure path keeps the table tiny.
  await query(`DELETE FROM login_limits WHERE window_start < now() - interval '1 day'`);
}

/**
 * A correct password: give back this attempt's count on the IP. Earlier wrong
 * ones stay. The account's count is left to expire: an admin on a clean IP
 * is never held up by it.
 */
export async function loginSucceeded(ip: string): Promise<void> {
  await query(`UPDATE login_limits SET n = greatest(n - 1, 0) WHERE key = $1`, [ipKey(ip)]);
}
