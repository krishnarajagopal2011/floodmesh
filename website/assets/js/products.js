/* The store's products, in rupees. Pack prices are worked out from the unit
   price and the volume discount tiers, so change those to reprice everything.
   Keep apps-script/Code.gs (UNIT_PRICE, TIERS, PRICES) in step.
   Text for each product lives in i18n.js under "p.<id>.*". */
window.FM_PRICING = (function () {
  var unitPrice = 6900;
  // Volume discount: the rate for the number of units in one pack or custom pack.
  var tiers = [
    { min: 250, pct: 30 },
    { min: 100, pct: 25 },
    { min: 50, pct: 20 },
    { min: 25, pct: 15 },
    { min: 10, pct: 10 },
  ];
  function pctFor(units) {
    for (var i = 0; i < tiers.length; i++) if (units >= tiers[i].min) return tiers[i].pct;
    return 0;
  }
  /** Price of one unit when buying `units` together, rounded to the rupee. */
  function perUnit(units) { return Math.round(unitPrice * (100 - pctFor(units)) / 100); }
  return { unitPrice: unitPrice, tiers: tiers, pctFor: pctFor, perUnit: perUnit };
})();

window.FM_PRODUCTS = (function (P) {
  // Product picture of the unit (a render, owner's decision 4 Oct), used on the
  // unit and pack cards. If it fails to load, the card keeps its placeholder.
  var UNIT_IMG = ["assets/img/unit.webp"];
  function pack(id, units, maxQty) {
    return { id: id, group: "packs", units: units, pct: P.pctFor(units), price: P.perUnit(units) * units, minQty: 1, maxQty: maxQty, bullets: 3, media: "product-" + id, image: UNIT_IMG };
  }
  return [
    { id: "unit", group: "units", price: P.unitPrice, minQty: 1, maxQty: 9, bullets: 3, media: "product-unit", image: UNIT_IMG },

    pack("street", 10, 10),          // 10% off: ₹62,100
    pack("neighbourhood", 25, 10),   // 15% off: ₹1,46,625
    pack("ward", 50, 10),            // 20% off: ₹2,76,000
    pack("area", 100, 5),            // 25% off: ₹5,17,500
    pack("village", 250, 4),         // 30% off: ₹12,07,500
    // Custom pack: the quantity is the number of units; the price per unit
    // follows the tier for that number (P.perUnit).
    { id: "custom", group: "packs", perUnit: true, price: P.perUnit(10), minQty: 10, maxQty: 500, bullets: 2, media: "product-custom", image: UNIT_IMG },

    { id: "antenna", group: "accessories", price: 6500, minQty: 1, maxQty: 20, bullets: 2, media: "product-antenna" },
    { id: "cable", group: "accessories", price: 750, minQty: 1, maxQty: 20, bullets: 2, media: "product-cable" },
  ];
})(window.FM_PRICING);

/** Price of one item of product p when the line's quantity is qty. */
window.FM_PRICE_EACH = function (p, qty) {
  return p.perUnit ? window.FM_PRICING.perUnit(qty) : p.price;
};
