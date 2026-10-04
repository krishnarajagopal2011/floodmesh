/**
 * POST /api/preorder: passes a pre-order from the store to the Google Apps
 * Script that writes it to the Sheet (apps-script/Code.gs).
 *
 * Why a function in between: the Apps Script URL and its key stay on the
 * server (Vercel environment variables), so nobody can post to the Sheet
 * directly, and Vercel's firewall can rate-limit this path by IP
 * (README.md, "Deploy on Vercel").
 *
 * Environment variables (Vercel -> Settings -> Environment Variables):
 *   ORDER_ENDPOINT  the Apps Script web-app URL (ends in /exec)
 *   ORDER_KEY       the same long random string as the script's KEY property
 */
module.exports = async function (req, res) {
  if (req.method !== "POST") {
    res.setHeader("Allow", "POST");
    return res.status(405).json({ ok: false, error: "method" });
  }
  if (!process.env.ORDER_ENDPOINT || !process.env.ORDER_KEY) {
    return res.status(503).json({ ok: false, error: "not-configured" });
  }
  const body = typeof req.body === "string" ? req.body : JSON.stringify(req.body || {});
  if (body.length > 8000) return res.status(413).json({ ok: false, error: "size" });
  try {
    const url = process.env.ORDER_ENDPOINT + "?k=" + encodeURIComponent(process.env.ORDER_KEY);
    // Apps Script answers with a redirect to the result; fetch follows it.
    const r = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "text/plain;charset=utf-8" },
      body,
      redirect: "follow",
    });
    const out = await r.json();
    res.setHeader("Cache-Control", "no-store");
    return res.status(200).json({ ok: !!out.ok, id: out.id, error: out.error });
  } catch (e) {
    return res.status(502).json({ ok: false, error: "upstream" });
  }
};
