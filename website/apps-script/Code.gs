/**
 * FloodMesh pre-orders: receives orders from the website (through the Vercel
 * function website/api/preorder.js) and appends a row to the "Pre-orders"
 * sheet, then emails a short notice. Setup: README.md here.
 *
 * A standalone script (script.google.com), not one bound to the Sheet: the
 * Sheet is named by the SHEET_ID script property, so it can be swapped for a
 * fresh copy when someone's details must be deleted (README, "Deleting
 * someone's details") without changing the web-app URL.
 *
 * Script properties (Project Settings -> Script properties):
 *   SHEET_ID      the pre-orders Sheet's ID (from its URL)
 *   KEY           a long random string, the same as ORDER_KEY on Vercel
 *   NOTIFY_EMAIL  where order notices go (optional)
 *
 * Prices are worked out here from UNIT_PRICE, TIERS and PRICES below, so a
 * changed price in the browser can't change what the Sheet records. Keep them
 * in step with website/assets/js/products.js.
 */

const SHEET_NAME = 'Pre-orders';
const UNIT_PRICE = 6900;
// Volume discount: the rate for the number of units in one pack or custom pack.
const TIERS = [[250, 30], [100, 25], [50, 20], [25, 15], [10, 10]];
function pctFor_(units) {
  for (const [min, pct] of TIERS) if (units >= min) return pct;
  return 0;
}
/** Price of one unit when buying `units` together, rounded to the rupee. */
function perUnit_(units) { return Math.round(UNIT_PRICE * (100 - pctFor_(units)) / 100); }

const PRICES = {                  // rupees per item; custom is priced per unit by perUnit_
  unit: UNIT_PRICE,
  street: perUnit_(10) * 10,          // 10% off: 62,100
  neighbourhood: perUnit_(25) * 25,   // 15% off: 1,46,625
  ward: perUnit_(50) * 50,            // 20% off: 2,76,000
  area: perUnit_(100) * 100,          // 25% off: 5,17,500
  village: perUnit_(250) * 250,       // 30% off: 12,07,500
  custom: null,
  antenna: 6500,
  cable: 750,
};
const UNITS_PER_ITEM = { unit: 1, street: 10, neighbourhood: 25, ward: 50, area: 100, village: 250, custom: 1, antenna: 0, cable: 0 };
const MIN_QTY = { custom: 10 };
const MAX_QTY = { unit: 9, street: 10, neighbourhood: 10, ward: 10, area: 5, village: 4, custom: 500, antenna: 20, cable: 20 };
const MAX_PER_PHONE_PER_HOUR = 5;
const FLOOD_PER_10_MIN = 30;      // beyond this, rows are marked "Check" and no email is sent

const HEADERS = [
  'Received at', 'Order ID', 'Status', 'Name', 'Phone', 'Email', 'Organisation',
  'Address', 'City', 'PIN', 'State', 'Items', 'FloodMesh units', 'Total (Rs)', 'Total shown in browser',
  'Language', 'Notes', 'Preview', 'Page',
];

function doPost(e) {
  const props = PropertiesService.getScriptProperties();
  // Only the Vercel function knows the key; posts straight to this URL are refused.
  if (!e || !e.parameter || !props.getProperty('KEY') || e.parameter.k !== props.getProperty('KEY')) {
    return json_({ ok: false, error: 'auth' });
  }
  let order;
  try { order = JSON.parse(e.postData.contents); } catch (err) { return json_({ ok: false, error: 'bad' }); }
  const problem = check_(order);
  if (problem) return json_({ ok: false, error: problem });

  const lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);
    const cache = CacheService.getScriptCache();
    const idKey = 'id:' + order.id;
    if (cache.get(idKey)) return json_({ ok: true, id: order.id });   // a retry of a saved order

    const phone = String(order.customer.phone);
    const seen = Number(cache.get('ph:' + phone) || 0);
    if (seen >= MAX_PER_PHONE_PER_HOUR) return json_({ ok: false, error: 'rate' });
    cache.put('ph:' + phone, String(seen + 1), 3600);
    const recent = Number(cache.get('all10m') || 0);
    cache.put('all10m', String(recent + 1), 600);
    const flood = recent >= FLOOD_PER_10_MIN;

    let units = 0;
    let total = 0;
    const items = order.items.map(function (it) {
      const qty = Number(it.qty);
      units += qty * UNITS_PER_ITEM[it.id];
      total += (it.id === 'custom' ? perUnit_(qty) : PRICES[it.id]) * qty;
      return qty + ' x ' + it.id;
    }).join(', ');

    const c = order.customer;
    sheet_().appendRow([
      new Date(), String(order.id), flood ? 'Check' : 'New', text_(c.name), "'" + phone, text_(c.email), text_(c.org),
      text_(c.address), text_(c.city), "'" + c.pin, text_(c.state), items, units, total, Number(order.total) || '',
      text_(order.lang), text_(c.notes), order.preview ? 'yes' : '', text_(order.page),
    ]);
    cache.put(idKey, '1', 21600);

    // The row is the record. The email is a best-effort notice with no
    // personal details, so a failed or over-quota email never loses an order
    // and no copies of names or addresses sit in a mailbox.
    const notify = props.getProperty('NOTIFY_EMAIL');
    if (notify && !flood) {
      try {
        if (MailApp.getRemainingDailyQuota() > 0) {
          MailApp.sendEmail(notify, 'FloodMesh pre-order ' + order.id,
            'Order: ' + order.id + '\nItems: ' + items + '  Total: Rs ' + total +
            '\nDetails are in the Sheet: ' + SpreadsheetApp.openById(props.getProperty('SHEET_ID')).getUrl());
        }
      } catch (mailErr) {
        console.error('notice email failed for ' + order.id + ': ' + mailErr);
      }
    }
    return json_({ ok: true, id: order.id });
  } catch (err) {
    console.error(err);
    return json_({ ok: false, error: 'server' });
  } finally {
    lock.releaseLock();
  }
}

function doGet() {
  return json_({ ok: true, service: 'floodmesh-preorders' });
}

function check_(o) {
  if (!o || typeof o !== 'object') return 'bad';
  if (!/^FM-\d{6}-[A-Z0-9]{5}$/.test(String(o.id))) return 'id';
  if (o.consent !== true) return 'consent';
  const c = o.customer || {};
  if (!c.name || String(c.name).length > 80) return 'name';
  if (!/^[6-9]\d{9}$/.test(String(c.phone))) return 'phone';
  if (!/^[1-9]\d{5}$/.test(String(c.pin))) return 'pin';
  if (!c.address || String(c.address).length > 300) return 'address';
  if (!c.city || !c.state) return 'place';
  if (!Array.isArray(o.items) || !o.items.length || o.items.length > 10) return 'items';
  const seenIds = {};
  for (const it of o.items) {
    if (!it || typeof it.id !== 'string' || !Object.prototype.hasOwnProperty.call(PRICES, it.id)) return 'item';
    if (seenIds[it.id]) return 'item';   // one line per product; the store never sends duplicates
    seenIds[it.id] = true;
    const q = Number(it.qty);
    if (!(q >= (MIN_QTY[it.id] || 1) && q <= MAX_QTY[it.id] && Math.floor(q) === q)) return 'qty';
  }
  return null;
}

function sheet_() {
  const ss = SpreadsheetApp.openById(PropertiesService.getScriptProperties().getProperty('SHEET_ID'));
  let sh = ss.getSheetByName(SHEET_NAME);
  if (!sh) sh = ss.insertSheet(SHEET_NAME);
  if (sh.getLastRow() === 0) {
    sh.appendRow(HEADERS);
    sh.setFrozenRows(1);
  }
  return sh;
}

/**
 * Text for a cell. A leading = + - @ (or tab or CR) would run as a formula in
 * Sheets, or in a spreadsheet program opening a CSV export. The first ' is the
 * Sheets text marker and isn't stored; the second stays in the value, so the
 * protection survives a CSV export.
 */
function text_(v) {
  const s = String(v == null ? '' : v).slice(0, 500);
  return /^[=+\-@\t\r]/.test(s) ? "''" + s : s;
}

function json_(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj)).setMimeType(ContentService.MimeType.JSON);
}
