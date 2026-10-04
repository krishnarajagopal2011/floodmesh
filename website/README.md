# FloodMesh website

A static site: home page, store with pre-orders, and privacy and pre-order
terms, in English, Tamil and Hindi (the Tamil and Hindi text is machine-assisted
and needs a native speaker's check before launch). No build step and no libraries.

| File | What it is |
|---|---|
| `index.html`, `store.html`, `privacy.html` | The pages. English text is written in the HTML |
| `assets/css/site.css` | All styling, light and dark |
| `assets/js/config.js` | **Settings to go live**: preview flag, order endpoint, contact details |
| `assets/js/products.js` | Products and prices: ₹6,900 a unit; volume discount 10% (10–24 units), 15% (25–49), 20% (50–99), 25% (100–249), 30% (250+), for packs of 10, 25, 50, 100 and 250 and custom packs from 10; rooftop antenna ₹6,500, longer antenna cable ₹750 |
| `assets/js/i18n.js` | Tamil and Hindi text, and English text the scripts create |
| `assets/js/site.js`, `store.js` | Language switch, cart, order form |
| `assets/js/mesh.js` | The animated street map in "How it works": a message to neighbours and an SOS to a rescue team |
| `api/preorder.js` | Vercel function that passes pre-orders to the Sheet script with a secret key |
| `apps-script/` | Google Sheet script that records pre-orders (setup in its README) |
| `vercel.json` | Clean URLs, security headers and caching for Vercel |
| `.vercelignore` | Keeps this README and `apps-script/` off the live site |

## Rules for the content

- **Name:** the product is FloodMesh, with no "by ..." line. The company,
  dVerse Technologies, appears only in the footer, plus where the law needs
  it (privacy page and the consent line on the order form).
- **Messages first, then SOS.** Neighbour-to-neighbour messages are half of
  what the unit does; don't let the SOS crowd them out.

- **No technical detail that helps someone copy the product.** No radio
  frequency, chip names, protocols, settings or circuit details, and no link
  to the code repository. Describe what it does for people, not how.
- **Claim only what has been measured.** Range figures come from
  `docs/field-tests.md`. Battery life says "designed to", because it hasn't
  been measured. Don't claim waterproofing until the case is tested.
- **Real footage for anything that shows results.** AI images only for mood
  and scenes. The owner decided (4 Oct 2026) that scene images carry no
  "Illustration" label or caption. Never an AI image of the product, and
  never on a product card.
- **Only what version 1 does:** an SOS (hold * and #, then pick the type of
  help) and short text messages. Leave out features dropped from version 1
  (`docs/architecture.md` §8).

## Go-live checklist

1. Set up the Sheet script, the Vercel environment variables and the
   firewall rule (`apps-script/README.md` and "Deploy on Vercel" below).
2. Replace the photo placeholders with real photos (list below).
3. A native speaker checks the Tamil and Hindi text in `i18n.js`.
4. A lawyer checks `privacy.html`, including the Data Protection Board
   sentence, and whether the DPDP Act requires the notice in Tamil and Hindi
   once the site is in those languages. If it does, add translations with
   English as the authoritative text.
5. Confirm with the owner: whether prices include GST (the site doesn't say),
   the length of the longer antenna cable, and "cancel any time before it
   ships" (FAQ and terms).
6. In the same commit: `preview: false` and `orderEndpoint: "/api/preorder"`
   in `config.js` (or test the endpoint first on a Vercel preview deployment
   of a branch).
7. Once the domain is known, make the `og:image` URLs in the three pages
   absolute (`https://<domain>/assets/img/og.png`); WhatsApp and LinkedIn
   previews need that.
8. Deploy (below), open `/store` in a private window and send one test
   pre-order. Check that `/README.md` and `/apps-script/Code.gs` return 404.

## Deploy on Vercel

New project → import the repository → **Root Directory: `website`** →
Framework preset: **Other** → no build command, no output directory. For now
the site uses Vercel's free `<project>.vercel.app` address; a custom domain
can be added later under Settings → Domains. `vercel.json` already sets clean URLs
(`/store` instead of `/store.html`; the pages link to `store.html`, which
costs one redirect but keeps local previews working), the security headers,
and caching (images for an hour; scripts and styles revalidated on every
load, so a price or setting change reaches everyone at once).

- **Environment variables:** `ORDER_ENDPOINT` and `ORDER_KEY`
  (`apps-script/README.md`, step 5).
- **Firewall (Pro):** Firewall → add a rule: Request Path equals
  `/api/preorder` → Rate limit, fixed window 600 s, 5 requests, keyed by IP,
  action Deny.
- The content security policy allows Google Fonts and the site's own
  function only. A new outside service (Razorpay checkout, Vercel Web
  Analytics or Speed Insights) needs its domain in `vercel.json` and a line on
  the privacy page first.
- HSTS deliberately leaves out `includeSubDomains`. If the site moves to the
  company's root domain, every subdomain would then have to serve HTTPS
  (Google Workspace custom URLs such as mail.<domain> don't). Add it only
  once the site has a domain of its own or every subdomain has been checked.

Preview locally with `npx serve website` or `vercel dev`; opening the files
directly also works, apart from the order endpoint.

## Media needed

Put files in `assets/img/`.
- Slots in the HTML (`story-scene`): add an `<img>` inside the `data-asset`
  figure.
- Store slots (`product-*`): set `image: "assets/img/<file>"` on that product
  in `products.js`; the card shows it. Optional alt text goes in `i18n.js` as
  `p.<id>.alt`.

| Slot | What | Source |
|---|---|---|
| `story-scene` | A flooded city street at dusk, ground floors under water, one lit window | AI scene (in place), no caption |
| `product-unit` | The unit in a hand, screen lit, plain background | **Real photo** |
| `product-street`, `product-neighbourhood`, `product-area`, `product-custom` | Boxed units stacked: 10, 25, 100 (or one photo of a stack for all four) | **Real photo** |
| `product-antenna` | The 1 m rooftop antenna mounted on a terrace railing, unit beside it | **Real photo** |
| `product-cable` | The cable coiled, both connectors visible | **Real photo** |
| `og.png` (1200 × 630) | Share image for WhatsApp, X and LinkedIn. A placeholder card is there now | Real photo + wordmark |
| Hero (optional) | Short loop of a real field test, 6–10 s, no sound | **Real video** |

### Prompts for AI scenes

For Higgsfield, or any image or video generator. Never show the FloodMesh
unit itself in AI output: product shots must be real photos. Drop finished files into the Google Drive
folder "FloodMesh website" and they can be placed from there.

- Water level: the owner wants severe floods, not knee-deep water. Add to
  every flood prompt: "ground floors completely submerged, brown floodwater
  up to the first-floor balconies and window sills, compound walls and
  gates fully under water, only upper storeys and roofs above the water".
- `story-scene` (still, 16:9 or 4:3): "Documentary photograph of a
  residential lane in Chennai in a severe flood at dusk, [water level as
  above], flat-roofed concrete houses with square metal grill balconies, a
  warm torch-lit upstairs window, light rain, overcast sky, no people, no
  text, natural colours." No caption on the page.
- Hero background loop (video, 6-8 s, 16:9, no sound, optional): "Slow aerial
  drift over a flooded Chennai neighbourhood at blue hour, rooftops and
  water tanks above the water line, a few lit windows, light rain,
  calm and quiet, documentary style, no text, no people close up."
- Rooftop mood (still, 16:9): "A terrace in a Chennai apartment block at
  dawn after heavy rain, small solar panel and a water tank, wet floor,
  clouds breaking, documentary photography." (Leave space on the right; a
  real photo of the rooftop unit goes on the product card, not here.)

Photos: at least 2000 px on the long side, daylight or a lit room, the unit's
screen on. Don't show the inside of the unit or any circuit board.

## How the language switch works

The HTML carries the English. Each translatable element has `data-i18n="key"`;
`site.js` swaps its text for `FM_I18N.ta[key]` or `FM_I18N.hi[key]`, and
falls back to English for any missing key. The choice is remembered in the
browser and can be forced with `?lang=ta` or `?lang=hi`. A language's
button appears only once its dictionary has text, and text that falls back to
English is marked `lang="en"` for screen readers. The privacy page stays in
English for now (see the go-live checklist).
