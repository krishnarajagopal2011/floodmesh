/* The store's products. Prices are in rupees, per unit, including GST.
   PLACEHOLDER PRICES: replace with real ones before setting preview: false.
   Text for each product lives in i18n.js under "p.<id>.*". */
window.FM_PRODUCTS = [
  { id: "household", price: 2999, maxQty: 50, bullets: 3, media: "product-household" },
  { id: "rooftop",   price: 6999, maxQty: 10, bullets: 3, media: "product-rooftop" },
  { id: "responder", price: 4999, maxQty: 20, bullets: 3, media: "product-responder", orgOnly: true },
  { id: "community", price: 64999, maxQty: 5, bullets: 3, media: "product-community",
    bundle: { household: 20, rooftop: 2 } },
];
