# Pre-orders to a Google Sheet

The store's order form posts to `/api/preorder`, a small Vercel function
(`website/api/preorder.js`). The function passes the order, with a secret key,
to a Google Apps Script, which adds a row to a Google Sheet and emails a short
notice. No payment is taken.

## Set up (about 15 minutes, once)

1. In Google Drive, create a Google Sheet named **FloodMesh pre-orders**.
   Copy its ID: the long part of its URL between `/d/` and `/edit`.
2. Go to **script.google.com → New project** (a standalone script, not
   Extensions → Apps Script inside the Sheet). Name it "FloodMesh pre-orders".
   Delete the sample code and paste in `Code.gs` from this folder.
3. **Project Settings → Script properties**, add:
   - `SHEET_ID`: the Sheet ID from step 1.
   - `KEY`: a long random string (for example 40 letters and digits from a
     password generator). Keep it secret.
   - `NOTIFY_EMAIL`: where order notices go, for example
     support@dverselabs.com. Optional.
4. **Deploy → New deployment → Web app.**
   - Execute as: **Me**
   - Who has access: **Anyone**. Posts without the key are refused.
   Approve the permissions it asks for: Sheets (to write the row) and email
   (for the notice).
5. In **Vercel → the website project → Settings → Environment Variables**, add:
   - `ORDER_ENDPOINT`: the web-app URL from step 4 (it ends in `/exec`).
   - `ORDER_KEY`: the same string as `KEY` in step 3.
   Redeploy the site so the function picks them up.
6. In `website/assets/js/config.js`, set `orderEndpoint: "/api/preorder"`.
   Do this in the same commit as `preview: false`, or test on a Vercel
   preview deployment of a branch first, so the live site never takes orders
   while it is still marked as a preview.
7. Send one test pre-order and check the row appears. Delete the test row.

The script creates the **Pre-orders** tab with headers on the first order. The
**Status** column starts as "New" ("Check" if orders arrive in a sudden flood);
change it by hand as you call people (Confirmed, Cancelled, Shipped).

**Check the Sheet every day.** It is the record. Notice emails are a
convenience: they carry no personal details, and they stop once the account's
daily mail quota is used up (100 a day on gmail.com, 1,500 on Workspace).

## When you change `Code.gs`

**Deploy → Manage deployments → edit (pencil) → Version: New version.** This
keeps the same URL. A brand-new deployment gets a new URL, which must then go
into `ORDER_ENDPOINT` on Vercel again.

## What it checks

- The key, so only the website's function can add rows.
- Phone (10-digit Indian mobile), PIN code, product IDs (one line per
  product) and quantities, and consent.
- It recalculates the total from its own `PRICES`, so the Sheet always shows
  the real price.
- At most 5 pre-orders per phone number per hour. A retry of an order already
  saved (same order ID) is accepted without adding a second row.
- More than 30 orders in 10 minutes: rows are marked "Check" and no email is
  sent.
- It escapes text that Sheets would otherwise run as a formula.

Vercel's firewall adds a per-IP limit in front (website/README.md).

## Taking orders out of the Sheet

Use **File → Download → Microsoft Excel (.xlsx)**. In a .csv download, cells
that started with `=`, `+`, `-` or `@` keep a leading apostrophe; don't let a
spreadsheet program run formulas from a CSV import.

## Deleting someone's details

Do this when a pre-order is cancelled before shipping (within 30 days, as the
privacy page promises), when someone asks for deletion, and at least every 4
weeks while any deletion is pending. Deleting a row is not enough: the Sheet's
version history keeps old values.

1. Delete their rows from the Sheet.
2. Delete any email or WhatsApp threads with their details.
3. **File → Make a copy** of the Sheet. A copy has no version history. Share
   the copy with the order team.
4. Put the copy's ID into the script's `SHEET_ID` property.
5. Delete the old file, and empty it from Drive's Trash.
6. Send one test pre-order to confirm it lands in the new file, then delete
   the test row.

The web-app URL stays the same throughout.

## Moving to Razorpay later

Keep this Sheet as the order record. A Razorpay step would create a payment
link for a confirmed row (from the Sheet, or from another Vercel function), so
the website form doesn't need to change until online payment is switched on.
Razorpay's checkout script would also need its domain added to the content
security policy in `website/vercel.json`.
