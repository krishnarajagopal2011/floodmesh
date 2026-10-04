/* FloodMesh website: language switch, cart badge, and the
   screen on the home page's unit illustration. No libraries. */
(function () {
  "use strict";

  var LANGS = ["en", "ta", "hi"];
  var originals = {};   // English text and markup captured from the page itself
  var chosen = "en";    // the language the visitor picked

  /* localStorage, with an in-memory copy for browsers that block it. With
     storage blocked, the cart and language last only until the page is left. */
  var mem = {}, memOnly = false;
  function store(key, value) {
    if (value === undefined) {
      if (!memOnly) { try { return window.localStorage.getItem(key); } catch (e) { memOnly = true; } }
      return Object.prototype.hasOwnProperty.call(mem, key) ? mem[key] : null;
    }
    mem[key] = String(value);
    if (!memOnly) { try { window.localStorage.setItem(key, value); } catch (e) { memOnly = true; } }
    return null;
  }

  /** A language is offered only once its dictionary has text. */
  function hasLang(lang) {
    var d = (window.FM_I18N || {})[lang];
    return lang === "en" || (!!d && Object.keys(d).length > 0);
  }

  function currentLang() {
    var fromUrl = null;
    try { fromUrl = new URLSearchParams(window.location.search).get("lang"); } catch (e) {}
    if (fromUrl && LANGS.indexOf(fromUrl) >= 0 && hasLang(fromUrl)) {
      store("fm-lang", fromUrl);   // a shared ?lang=ta link carries to the next page
      return fromUrl;
    }
    var lang = store("fm-lang") || "en";
    return LANGS.indexOf(lang) >= 0 && hasLang(lang) ? lang : "en";
  }

  function has(key, lang) {
    var d = (window.FM_I18N || {})[lang];
    return !!d && d[key] != null;
  }

  /** Text for a key: the chosen language, else the English dictionary, else the page's own English. */
  function t(key, lang) {
    lang = lang || chosen;
    var dict = window.FM_I18N || {};
    if (has(key, lang)) return dict[lang][key];
    if (dict.en && dict.en[key] != null) return dict.en[key];
    return originals[key] != null ? originals[key] : key;
  }

  /** Mark text that fell back to English, so screen readers and :lang() CSS get it right. */
  function tag(el, key, lang) {
    if (lang === "en") el.removeAttribute("lang");
    else el.setAttribute("lang", has(key, lang) ? lang : "en");
  }

  function applyLang(lang) {
    chosen = lang;
    document.documentElement.lang = lang;
    document.querySelectorAll("[data-i18n]").forEach(function (el) {
      var key = el.getAttribute("data-i18n");
      if (!(key in originals)) originals[key] = el.textContent;
      el.textContent = t(key, lang);
      tag(el, key, lang);
    });
    document.querySelectorAll("[data-i18n-html]").forEach(function (el) {
      var key = el.getAttribute("data-i18n-html");
      if (!(key in originals)) originals[key] = el.innerHTML;
      // Only our own dictionary goes in here, never user input.
      el.innerHTML = t(key, lang);
      tag(el, key, lang);
    });
    document.querySelectorAll("[data-lang]").forEach(function (b) {
      b.setAttribute("aria-pressed", String(b.getAttribute("data-lang") === lang));
    });
    document.dispatchEvent(new CustomEvent("fm:lang", { detail: { lang: lang } }));
  }

  /** The stored cart, keeping only products that exist, at quantities they allow. */
  function readCart() {
    var raw;
    try { raw = JSON.parse(store("fm-cart") || "{}") || {}; } catch (e) { raw = {}; }
    var cart = {};
    (window.FM_PRODUCTS || []).forEach(function (p) {
      var q = Math.floor(Number(raw[p.id]));
      if (q >= (p.minQty || 1)) cart[p.id] = Math.min(q, p.maxQty);
    });
    return cart;
  }

  function updateCount() {
    var cart = readCart();
    // The number of different products in the cart (a custom pack of 40 units counts once).
    var n = Object.keys(cart).length;
    document.querySelectorAll("[data-cart-count]").forEach(function (el) {
      el.textContent = String(n);
      el.hidden = n === 0;
    });
  }

  function showContact() {
    var c = window.FM_CONFIG || {};
    document.querySelectorAll("[data-contact]").forEach(function (el) {
      el.textContent = "";
      var parts = [];
      if (c.contactPhone) parts.push(["tel:" + c.contactPhone.replace(/[^\d+]/g, ""), c.contactPhone]);
      if (c.contactEmail) parts.push(["mailto:" + c.contactEmail, c.contactEmail]);
      parts.forEach(function (part, i) {
        if (i) el.appendChild(document.createTextNode(" \u00b7 "));
        var a = document.createElement("a");
        a.href = part[0];
        a.textContent = part[1];
        el.appendChild(a);
      });
      el.hidden = parts.length === 0;
    });
  }

  /* The unit illustration plays the SOS from press to "Help is coming".
     It rests on the last frame, which is also what shows without scripts. */
  function playScreen() {
    var screen = document.querySelector("[data-screen]");
    if (!screen) return;
    if (window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    // The screen shows the unit's own English words. It rests on frame 0, a
    // neighbour's message, which is also the markup shown without scripts.
    var frames = [
      { tag: "MSG", head: "FROM B-14", lines: ["WATER AT OUR GATE.", "ALL SAFE UPSTAIRS."] },
      { lines: ["TO ALL NEARBY:", "ANY BOAT ON", "3RD STREET?"] },
      { lines: ["TO ALL NEARBY"], big: "SENT" },
      { lines: ["SOS: PICK HELP TYPE", "1 MED 2 EVAC 3 HAZ", "4 FOOD 0 GENERAL"] },
      { tag: "SOS", head: "MEDICAL", lines: ["SENDING..."] },
      { tag: "SOS", head: "MEDICAL", big: "DELIVERED" },
      { tag: "SOS", head: "MEDICAL", lines: ["DELIVERED"], big: "HELP IS COMING" },
    ];
    var holds = [5200, 2600, 1800, 2600, 1800, 2200, 4000];
    var i = 0;

    function line(text, cls) {
      var el = document.createElement("span");
      el.className = "line" + (cls ? " " + cls : "");
      if (text != null) el.textContent = text;
      return el;
    }

    function render(frame) {
      screen.textContent = "";
      if (frame.tag) {
        var first = line(null);
        var inv = document.createElement("span");
        inv.className = "inv";
        inv.textContent = frame.tag;
        first.appendChild(inv);
        first.appendChild(document.createTextNode(" " + frame.head));
        screen.appendChild(first);
      }
      (frame.lines || []).forEach(function (text) { screen.appendChild(line(text)); });
      if (frame.big) screen.appendChild(line(frame.big, "big"));
    }

    function next() {
      if (document.hidden) { window.setTimeout(next, 1000); return; }
      i = (i + 1) % frames.length;
      render(frames[i]);
      window.setTimeout(next, holds[i]);
    }
    window.setTimeout(next, holds[0]);
  }

  window.FM = { t: t, store: store, readCart: readCart, updateCount: updateCount, lang: function () { return chosen; } };

  /* The header's real height, for anchor offsets and the sticky order panel. */
  function trackHeader() {
    var head = document.querySelector(".site-head");
    if (!head) return;
    var setH = function () { document.documentElement.style.setProperty("--head-h", head.offsetHeight + "px"); };
    setH();
    if ("ResizeObserver" in window) new ResizeObserver(setH).observe(head);
  }

  /* Phone menu: the links and the pre-order button open under the header (site.css, max-width 760px). */
  function setupMenu() {
    var head = document.querySelector(".site-head");
    var btn = document.querySelector(".menu-btn");
    if (!head || !btn) return;
    var set = function (open) {
      head.classList.toggle("menu-open", open);
      btn.setAttribute("aria-expanded", open ? "true" : "false");
    };
    btn.addEventListener("click", function () { set(!head.classList.contains("menu-open")); });
    head.addEventListener("click", function (e) { if (e.target.closest(".nav a, .cart-link")) set(false); });
    document.addEventListener("keydown", function (e) {
      if (e.key === "Escape" && head.classList.contains("menu-open")) { set(false); btn.focus(); }
    });
    var wide = window.matchMedia("(min-width: 761px)");
    var onWide = function () { if (wide.matches) set(false); };
    if (wide.addEventListener) wide.addEventListener("change", onWide); else if (wide.addListener) wide.addListener(onWide);
  }

  document.addEventListener("DOMContentLoaded", function () {
    var c = window.FM_CONFIG || {};
    setupMenu();
    document.querySelectorAll("[data-preview-only]").forEach(function (el) {
      el.hidden = !c.preview;
    });
    document.querySelectorAll("[data-lang]").forEach(function (b) {
      b.hidden = !hasLang(b.getAttribute("data-lang"));
    });
    document.querySelectorAll(".lang").forEach(function (g) {
      g.hidden = !g.querySelector("[data-lang]:not([hidden]):not([data-lang='en'])");
    });
    document.querySelectorAll("[data-lang]").forEach(function (b) {
      b.addEventListener("click", function () {
        var lang = b.getAttribute("data-lang");
        store("fm-lang", lang);
        applyLang(lang);
      });
    });
    applyLang(currentLang());
    trackHeader();
    updateCount();
    showContact();
    playScreen();
    document.addEventListener("fm:cart", updateCount);
    window.addEventListener("storage", function (e) { if (e.key === "fm-cart") updateCount(); });
  });
})();
