# Pre-orders to a Google Sheet

The store's order form sends each pre-order to a small Google Apps Script,
which adds a row to a Google Sheet and emails you. No payment is taken.

## Set up (about 10 minutes, once)

1. In Google Drive, create a Google Sheet named **FloodMesh pre-orders**.
2. In the Sheet: **Extensions → Apps Script**. Delete the sample code and
   paste in `Code.gs` from this folder.
3. At the top of `Code.gs`, set `NOTIFY_EMAIL` to the address that should get
   an email for every pre-order (for example support@dverselabs.com). If the
   prices change, change `PRICES` here and in `website/assets/js/products.js`.
4. **Deploy → New deployment → Web app.**
   - Execute as: **Me**
   - Who has access: **Anyone**
   Approve the permissions it asks for (Sheets and email).
5. Copy the web-app URL (it ends in `/exec`) into `orderEndpoint` in
   `website/assets/js/config.js`.
6. Send one test pre-order from the site and check the row appears. Delete
   the test row.

The script creates the **Pre-orders** tab with headers on the first order. The
**Status** column starts as "New"; change it by hand as you call people
(Confirmed, Cancelled, Shipped).

## When you change `Code.gs`

**Deploy → Manage deployments → edit (pencil) → Version: New version.** This
keeps the same URL. A brand-new deployment gets a new URL, which must then go
into `config.js` again.

## What it checks

Phone (10-digit Indian mobile), PIN code, product IDs and quantities, and
consent. It recalculates the total from its own `PRICES`, so the Sheet always shows the real price. It
accepts at most 5 pre-orders per phone number per hour, and it escapes text
that Sheets would otherwise run as a formula.

## Moving to Razorpay later

Keep this Sheet as the order record. A Razorpay step would create a payment
link for a confirmed row (from the Sheet, or from a small Vercel function), so
the website form doesn't need to change until online payment is switched on.
