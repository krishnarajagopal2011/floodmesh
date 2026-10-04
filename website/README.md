# FloodMesh website

A static site: home page, store with pre-orders, and privacy and pre-order
terms, in English, Tamil and Hindi. No build step and no libraries.

| File | What it is |
|---|---|
| `index.html`, `store.html`, `privacy.html` | The pages. English text is written in the HTML |
| `assets/css/site.css` | All styling, light and dark |
| `assets/js/config.js` | **Settings to go live**: preview flag, order endpoint, contact details |
| `assets/js/products.js` | Products and prices: ₹6,900 a unit, packs of 10, 25, 100 or any number from 10 at 10% off, rooftop antenna ₹6,500, longer antenna cable ₹750 |
| `assets/js/i18n.js` | Tamil and Hindi text, and English text the scripts create |
| `assets/js/site.js`, `store.js` | Language switch, cart, order form |
| `apps-script/` | Google Sheet that receives pre-orders (setup in its README) |
| `vercel.json` | Clean URLs and security headers for Vercel |

## Rules for the content

- **No technical detail that helps someone copy the product.** No radio
  frequency, chip names, protocols, settings or circuit details, and no link
  to the code repository. Describe what it does for people, not how.
- **Claim only what has been measured.** Range figures come from
  `docs/field-tests.md`. Battery life says "designed to", because it hasn't
  been measured. Don't claim waterproofing until the case is tested.
- **Real footage for anything that shows results.** AI images only for mood
  and scenes, labelled "Illustration". Never an AI image of the product
  presented as a photo, and never on a product card once taking orders.
- **No voice notes.** Voice is not in version 1.

## Go-live checklist

1. Set up the Sheet (`apps-script/README.md`) and put its URL in
   `orderEndpoint` in `config.js`.
2. Replace the photo placeholders with real photos (list below).
3. A native speaker checks the Tamil and Hindi text in `i18n.js`.
4. A lawyer checks `privacy.html`.
5. Confirm with the owner: whether prices include GST (the site doesn't say),
   the length of the longer antenna cable, and "cancel any time before it
   ships" (FAQ and terms).
6. Set `preview: false` in `config.js`.
7. Once the domain is known, make the `og:image` URLs in the three pages
   absolute (`https://<domain>/assets/img/og.png`); WhatsApp and LinkedIn
   previews need that.
8. Deploy (below) and send one test pre-order.

## Deploy on Vercel

New project → import the repository → **Root Directory: `website`** →
Framework preset: **Other** → no build command, no output directory. For now
the site uses Vercel's free `<project>.vercel.app` address; a custom domain
can be added later under Settings → Domains. `vercel.json` already sets clean URLs
(`/store` instead of `/store.html`) and the security headers.

The content security policy allows Google Fonts and the Google Apps Script
order endpoint only. A new outside service (Razorpay checkout, analytics)
needs its domain added to `vercel.json` first.

## Media needed

Each slot is a `data-asset` element in the HTML (or `media` in
`products.js`). Put files in `assets/img/` and add an `<img>` inside the slot.

| Slot | What | Source |
|---|---|---|
| `story-scene` | A flooded city street at dusk, water at knee height, apartment blocks, one lit window | AI illustration, labelled |
| `product-unit` | The unit in a hand, screen lit, plain background | **Real photo** |
| `product-street`, `product-neighbourhood`, `product-area`, `product-custom` | Boxed units stacked: 10, 25, 100 (or one photo of a stack for all four) | **Real photo** |
| `product-antenna` | The 1 m rooftop antenna mounted on a terrace railing, unit beside it | **Real photo** |
| `product-cable` | The cable coiled, both connectors visible | **Real photo** |
| `og.png` (1200 × 630) | Share image for WhatsApp, X and LinkedIn. A placeholder card is there now | Real photo + wordmark |
| Hero (optional) | Short loop of a real field test, 6–10 s, no sound | **Real video** |

Photos: at least 2000 px on the long side, daylight or a lit room, the unit's
screen on. Don't show the inside of the unit or any circuit board.

## How the language switch works

The HTML carries the English. Each translatable element has `data-i18n="key"`;
`site.js` swaps its text for `FM_I18N.ta[key]` or `FM_I18N.hi[key]`, and
falls back to English for any missing key. The choice is remembered in the
browser and can be forced with `?lang=ta` or `?lang=hi`. The privacy page
stays in English on purpose.
