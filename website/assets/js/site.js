/* FloodMesh website: language switch, preview ribbon, cart badge, and the
   screen on the home page's unit illustration. No libraries. */
(function () {
  "use strict";

  var LANGS = ["en", "ta", "hi"];
  var originals = {};   // English text and markup captured from the page itself

  function store(key, value) {
    try {
      if (value === undefined) return window.localStorage.getItem(key);
      window.localStorage.setItem(key, value);
    } catch (e) { /* private window or blocked storage: the page still works */ }
    return null;
  }

  function currentLang() {
    var fromUrl = null;
    try { fromUrl = new URLSearchParams(window.location.search).get("lang"); } catch (e) {}
    var lang = fromUrl || store("fm-lang") || "en";
    return LANGS.indexOf(lang) >= 0 ? lang : "en";
  }

  /** Text for a key: the chosen language, else the English dictionary, else the page's own English. */
  function t(key, lang) {
    lang = lang || document.documentElement.lang || "en";
    var dict = window.FM_I18N || {};
    if (dict[lang] && dict[lang][key] != null) return dict[lang][key];
    if (dict.en && dict.en[key] != null) return dict.en[key];
    return originals[key] != null ? originals[key] : key;
  }

  function applyLang(lang) {
    document.documentElement.lang = lang;
    document.querySelectorAll("[data-i18n]").forEach(function (el) {
      var key = el.getAttribute("data-i18n");
      if (!(key in originals)) originals[key] = el.textContent;
      el.textContent = t(key, lang);
    });
    document.querySelectorAll("[data-i18n-html]").forEach(function (el) {
      var key = el.getAttribute("data-i18n-html");
      if (!(key in originals)) originals[key] = el.innerHTML;
      // Only our own dictionary goes in here, never user input.
      el.innerHTML = t(key, lang);
    });
    document.querySelectorAll("[data-lang]").forEach(function (b) {
      b.setAttribute("aria-pressed", String(b.getAttribute("data-lang") === lang));
    });
    document.dispatchEvent(new CustomEvent("fm:lang", { detail: { lang: lang } }));
  }

  function readCart() {
    try { return JSON.parse(store("fm-cart") || "{}") || {}; } catch (e) { return {}; }
  }

  function updateCount() {
    var cart = readCart();
    var n = Object.keys(cart).reduce(function (s, k) { return s + (cart[k] | 0); }, 0);
    document.querySelectorAll("[data-cart-count]").forEach(function (el) {
      el.textContent = String(n);
      el.hidden = n === 0;
    });
  }

  function showContact() {
    var c = window.FM_CONFIG || {};
    var parts = [];
    if (c.contactPhone) parts.push(c.contactPhone);
    if (c.contactEmail) parts.push(c.contactEmail);
    document.querySelectorAll("[data-contact]").forEach(function (el) {
      el.textContent = parts.join(" · ");
      el.hidden = parts.length === 0;
    });
  }

  /* The unit illustration plays the SOS from press to "Help is coming".
     It rests on the last frame, which is also what shows without scripts. */
  function playScreen() {
    var screen = document.querySelector("[data-screen]");
    if (!screen) return;
    if (window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    var frames = [
      { lines: ["SOS: PICK HELP TYPE", "1 MED 2 EVAC 3 HAZ", "4 FOOD 0 GENERAL"] },
      { sos: "MEDICAL", lines: ["SENDING..."] },
      { sos: "MEDICAL", big: "DELIVERED" },
      { sos: "MEDICAL", lines: ["DELIVERED"], big: "HELP IS COMING" },
    ];
    var holds = [2600, 1800, 2400, 5200];
    var i = frames.length - 1;

    function line(text, cls) {
      var el = document.createElement("span");
      el.className = "line" + (cls ? " " + cls : "");
      if (text != null) el.textContent = text;
      return el;
    }

    function render(frame) {
      screen.textContent = "";
      if (frame.sos) {
        var first = line(null);
        var inv = document.createElement("span");
        inv.className = "inv";
        inv.textContent = "SOS";
        first.appendChild(inv);
        first.appendChild(document.createTextNode(" " + frame.sos));
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
    window.setTimeout(next, 4000);
  }

  window.FM = { t: t, store: store, readCart: readCart, updateCount: updateCount };

  document.addEventListener("DOMContentLoaded", function () {
    var c = window.FM_CONFIG || {};
    document.querySelectorAll("[data-preview-ribbon], [data-preview-only]").forEach(function (el) {
      el.hidden = !c.preview;
    });
    document.querySelectorAll("[data-lang]").forEach(function (b) {
      b.addEventListener("click", function () {
        var lang = b.getAttribute("data-lang");
        store("fm-lang", lang);
        applyLang(lang);
      });
    });
    applyLang(currentLang());
    updateCount();
    showContact();
    playScreen();
    document.addEventListener("fm:cart", updateCount);
    window.addEventListener("storage", function (e) { if (e.key === "fm-cart") updateCount(); });
  });
})();
