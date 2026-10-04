/**
 * FloodMesh pre-orders: receives the website's order form and appends a row to
 * the "Pre-orders" sheet, then emails a notification. Setup: README.md here.
 *
 * Prices are checked here against PRICES below, so a changed price in the
 * browser can't change what the Sheet records. Keep PRICES in step with
 * website/assets/js/products.js.
 */

const SHEET_NAME = 'Pre-orders';
const NOTIFY_EMAIL = '';          // where order notifications go; empty = no email
const PRICES = {                  // rupees, per item (custom: per unit)
  unit: 6900,
  street: 62100,          // 10 units at 10% off
  neighbourhood: 155250,  // 25 units at 10% off
  area: 621000,           // 100 units at 10% off
  custom: 6210,           // per unit, 10% off, quantity = units
  antenna: 6500,
  cable: 750,
};
const UNITS_PER_ITEM = { unit: 1, street: 10, neighbourhood: 25, area: 100, custom: 1, antenna: 0, cable: 0 };
const MIN_QTY = { custom: 10 };
const MAX_QTY = { unit: 9, street: 10, neighbourhood: 10, area: 5, custom: 500, antenna: 20, cable: 20 };
const MAX_PER_PHONE_PER_HOUR = 5;

const HEADERS = [
  'Received at', 'Order ID', 'Status', 'Name', 'Phone', 'Email', 'Organisation',
  'Address', 'City', 'PIN', 'State', 'Items', 'FloodMesh units', 'Total (Rs)', 'Total shown in browser',
  'Language', 'Notes', 'Preview', 'Page',
];

function doPost(e) {
  const lock = LockService.getScriptLock();
  try {
    lock.waitLock(10000);
    const order = JSON.parse(e.postData.contents);
    const problem = check_(order);
    if (problem) return json_({ ok: false, error: problem });

    const phone = String(order.customer.phone);
    const cache = CacheService.getScriptCache();
    const seen = Number(cache.get('ph:' + phone) || 0);
    if (seen >= MAX_PER_PHONE_PER_HOUR) return json_({ ok: false, error: 'rate' });
    cache.put('ph:' + phone, String(seen + 1), 3600);

    let units = 0;
    let total = 0;
    const items = order.items.map(function (it) {
      const qty = Math.floor(Number(it.qty));
      units += qty * UNITS_PER_ITEM[it.id];
      total += PRICES[it.id] * qty;
      return qty + ' x ' + it.id;
    }).join(', ');

    const c = order.customer;
    sheet_().appendRow([
      new Date(), String(order.id), 'New', text_(c.name), "'" + phone, text_(c.email), text_(c.org),
      text_(c.address), text_(c.city), "'" + c.pin, text_(c.state), items, units, total, Number(order.total) || '',
      text_(order.lang), text_(c.notes), order.preview ? 'yes' : '', text_(order.page),
    ]);

    if (NOTIFY_EMAIL) {
      MailApp.sendEmail(NOTIFY_EMAIL, 'FloodMesh pre-order ' + order.id + ' (' + items + ')',
        [
          'Order: ' + order.id,
          'Items: ' + items + '  Total: Rs ' + total,
          'Name: ' + c.name + '  Phone: ' + phone + (c.email ? '  Email: ' + c.email : ''),
          c.org ? 'Organisation: ' + c.org : '',
          'Address: ' + c.address + ', ' + c.city + ' ' + c.pin + ', ' + c.state,
          c.notes ? 'Notes: ' + c.notes : '',
        ].filter(String).join('\n'));
    }
    return json_({ ok: true, id: order.id });
  } catch (err) {
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
  for (const it of o.items) {
    if (!Object.prototype.hasOwnProperty.call(PRICES, String(it.id))) return 'item';
    const q = Number(it.qty);
    if (!(q >= (MIN_QTY[it.id] || 1) && q <= MAX_QTY[it.id] && Math.floor(q) === q)) return 'qty';
  }
  return null;
}

function sheet_() {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  let sh = ss.getSheetByName(SHEET_NAME);
  if (!sh) sh = ss.insertSheet(SHEET_NAME);
  if (sh.getLastRow() === 0) {
    sh.appendRow(HEADERS);
    sh.setFrozenRows(1);
  }
  return sh;
}

/** Text for a cell. A leading = + - or @ would make Sheets run it as a formula. */
function text_(v) {
  const s = String(v == null ? '' : v).slice(0, 500);
  return /^[=+\-@]/.test(s) ? "'" + s : s;
}

function json_(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj)).setMimeType(ContentService.MimeType.JSON);
}
