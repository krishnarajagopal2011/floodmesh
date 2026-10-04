/* The store's products, in rupees. Pack prices are worked out from the unit
   price and the pack discount, so change those two numbers to reprice
   everything. Keep apps-script/Code.gs PRICES in step.
   Text for each product lives in i18n.js under "p.<id>.*". */
window.FM_PRICING = { unitPrice: 6900, packDiscount: 0.10 };

window.FM_PRODUCTS = (function (P) {
  var packUnit = Math.round(P.unitPrice * (1 - P.packDiscount));   // 6210
  return [
    { id: "unit", group: "units", price: P.unitPrice, minQty: 1, maxQty: 9, bullets: 3, media: "product-unit" },

    { id: "street", group: "packs", units: 10, price: packUnit * 10, minQty: 1, maxQty: 10, bullets: 3, media: "product-street" },
    { id: "neighbourhood", group: "packs", units: 25, price: packUnit * 25, minQty: 1, maxQty: 10, bullets: 3, media: "product-neighbourhood" },
    { id: "area", group: "packs", units: 100, price: packUnit * 100, minQty: 1, maxQty: 5, bullets: 3, media: "product-area" },
    // Custom pack: the quantity is the number of units, at the pack price per unit.
    { id: "custom", group: "packs", perUnit: true, price: packUnit, minQty: 10, maxQty: 500, bullets: 2, media: "product-custom" },

    { id: "antenna", group: "accessories", price: 6500, minQty: 1, maxQty: 20, bullets: 2, media: "product-antenna" },
    { id: "cable", group: "accessories", price: 750, minQty: 1, maxQty: 20, bullets: 2, media: "product-cable" },
  ];
})(window.FM_PRICING);
