/* FloodMesh store: catalogue, cart (kept in this browser), and the pre-order
   form. Orders go to FM_CONFIG.orderEndpoint: /api/preorder, a small Vercel
   function that passes them to the Google Apps Script behind the pre-orders
   Sheet. No payment is taken. */
(function () {
  "use strict";

  var T = function (key) { return window.FM.t(key); };
  var products = window.FM_PRODUCTS || [];
  var config = window.FM_CONFIG || {};
  var byId = {};
  products.forEach(function (p) { byId[p.id] = p; });

  var money = new Intl.NumberFormat("en-IN", { style: "currency", currency: "INR", maximumFractionDigits: 0 });

  // ------------------------------------------------------------- cart
  // site.js's readCart already drops unknown products and clamps quantities.
  function readCart() { return window.FM.readCart(); }
  function writeCart(cart) {
    window.FM.store("fm-cart", JSON.stringify(cart));
    document.dispatchEvent(new CustomEvent("fm:cart"));
  }
  function cartLines(cart) {
    return products.filter(function (p) { return cart[p.id]; }).map(function (p) {
      var each = window.FM_PRICE_EACH(p, cart[p.id]);   // a custom pack's price per unit follows its size
      return { id: p.id, name: T("p." + p.id + ".name"), qty: cart[p.id], unitPrice: each, lineTotal: each * cart[p.id],
               units: p.perUnit ? cart[p.id] : (p.units || (p.group === "units" ? 1 : 0)) * cart[p.id] };
    });
  }
  function cartTotal(lines) { return lines.reduce(function (s, l) { return s + l.lineTotal; }, 0); }

  // ------------------------------------------------------------- rendering
  function el(tag, attrs, text) {
    var node = document.createElement(tag);
    Object.keys(attrs || {}).forEach(function (k) { node.setAttribute(k, attrs[k]); });
    if (text != null) node.textContent = text;
    return node;
  }

  /** The text for a key, or null if no language has it. */
  function opt(key) { var v = T(key); return v === key ? null : v; }

  function pctText(pct) { return T("store.pct").replace("{pct}", pct); }

  function saveText(amount) { return T("store.save").replace("{amount}", money.format(amount)); }

  /** The small line under a price: how the total for `qty` is made up, and
      the saving on packs. `each` is the price of one item at that quantity. */
  function priceNote(p, qty, each) {
    var unitPrice = window.FM_PRICING.unitPrice;
    if (p.perUnit) {
      return T("store.units").replace("{n}", qty) + " \u00d7 " + money.format(each) + " \u00b7 " +
        pctText(window.FM_PRICING.pctFor(qty)) + " \u00b7 " + saveText((unitPrice - each) * qty);
    }
    if (p.units) {
      return (qty > 1 ? qty + " \u00d7 " + money.format(each) : T("store.perpack")) + " \u00b7 " +
        pctText(p.pct) + " \u00b7 " + saveText((unitPrice * p.units - each) * qty);
    }
    return qty > 1 ? qty + " \u00d7 " + money.format(each) : T("store.each");
  }

  function productCard(p, keep) {
    var title = "t-" + p.id;
    var card = el("article", { class: "product", id: p.id, "aria-labelledby": title });

    var media = el("figure", { class: "media-slot", "data-asset": p.media });
    var srcs = [].concat(p.image || []);   // candidate files, first that loads wins
    var placeholder = function () { media.appendChild(el("span", { class: "tag" }, T("media.product"))); };
    if (srcs.length) {
      var img = el("img", { src: srcs[0], class: "product-photo", alt: opt("p." + p.id + ".alt") || T("p." + p.id + ".name"), loading: "lazy", decoding: "async", width: "1200", height: "1200" });
      var tried = 0;
      img.addEventListener("error", function () {
        tried += 1;
        if (tried < srcs.length) img.src = srcs[tried];
        else { img.remove(); placeholder(); }
      });
      media.appendChild(img);
    } else {
      placeholder();
    }
    card.appendChild(media);

    var body = el("div", { class: "product-body" });
    var badge = opt("p." + p.id + ".badge");
    if (badge) body.appendChild(el("span", { class: "badge" }, badge));
    body.appendChild(el("h3", { id: title }, T("p." + p.id + ".name")));
    body.appendChild(el("p", { class: "tagline" }, T("p." + p.id + ".tagline")));
    var ul = el("ul");
    for (var b = 1; b <= p.bullets; b++) ul.appendChild(el("li", null, T("p." + p.id + ".b" + b)));
    body.appendChild(ul);
    var hint = opt("p." + p.id + ".hint");
    if (hint) body.appendChild(el("p", { class: "hint-line" }, hint));
    card.appendChild(body);

    var foot = el("div", { class: "product-foot" });
    var price = el("p", { class: "price", "aria-live": "polite", "aria-atomic": "true" });
    // The full price, struck through, when a pack's discount applies. Screen readers
    // skip it and hear the saving in the line underneath instead.
    var was = el("s", { class: "was", "aria-hidden": "true" });
    var priceNum = el("span");
    var priceSmall = el("small");
    price.appendChild(was); price.appendChild(priceNum); price.appendChild(priceSmall);
    foot.appendChild(price);

    // Each control is described by the product name, so a screen reader's
    // list of controls says which product it belongs to.
    var row = el("div", { class: "add-row" });
    var stepper = el("div", { class: "stepper" });
    var qid = "q-" + p.id;
    var min = p.minQty || 1;
    var clamp = function (v) { return Math.max(min, Math.min(p.maxQty, parseInt(v, 10) || min)); };
    var minus = el("button", { type: "button", "aria-label": T("store.less"), "aria-describedby": title }, "−");
    var input = el("input", { id: qid, "data-qty": p.id, type: "number", inputmode: "numeric", min: String(min), max: String(p.maxQty), value: keep[qid] || String(min), "aria-label": T(p.perUnit ? "store.qtyunits" : "store.qty"), "aria-describedby": title });
    var plus = el("button", { type: "button", "aria-label": T("store.more"), "aria-describedby": title }, "+");
    minus.addEventListener("click", function () { input.value = String(clamp((parseInt(input.value, 10) || min) - 1)); showPrice(); });
    plus.addEventListener("click", function () { input.value = String(clamp((parseInt(input.value, 10) || 0) + 1)); showPrice(); });
    input.addEventListener("change", function () { input.value = String(clamp(input.value)); showPrice(); });
    // The price follows the quantity: the total for what is chosen, with how
    // it is made up underneath. A custom pack's price per unit also changes
    // with the number of units. A short highlight marks each change.
    var shown = null;
    function showPrice() {
      var qty = clamp(input.value);
      var each = window.FM_PRICE_EACH(p, qty);
      var total = each * qty;
      var full = window.FM_PRICING.unitPrice * (p.perUnit ? qty : (p.units || 0) * qty);
      was.textContent = full > total ? money.format(full) : "";
      was.hidden = !(full > total);
      priceNum.textContent = money.format(total);
      priceSmall.textContent = priceNote(p, qty, each);
      if (shown !== null && shown !== total) {
        price.classList.remove("bump");
        void price.offsetWidth;   // restart the highlight on quick repeated clicks
        price.classList.add("bump");
      }
      shown = total;
    }
    input.addEventListener("input", showPrice);
    showPrice();
    stepper.appendChild(minus); stepper.appendChild(input); stepper.appendChild(plus);
    var addId = "add-" + p.id;
    var add = el("button", { type: "button", id: addId, "aria-labelledby": addId + " " + title, class: "btn btn-line btn-small" }, T("store.add"));
    var note = el("p", { class: "cap-note", "aria-live": "polite" });
    add.addEventListener("click", function () {
      var q = clamp(input.value);
      var cart = readCart();
      var before = cart[p.id] || 0;
      var after = Math.min(p.maxQty, before + q);
      cart[p.id] = after;
      writeCart(cart);
      input.value = String(min);
      showPrice();
      // Tell the buyer when the per-order cap kept some of the quantity out.
      var capKey = p.perUnit ? "store.maxunits" : (p.id === "unit" ? "store.max.unit" : "store.max");
      note.textContent = after - before < q ? T(capKey).replace("{n}", p.maxQty) : "";
      add.textContent = T("store.added");
      window.clearTimeout(add._t);
      add._t = window.setTimeout(function () { add.textContent = T("store.add"); }, 1600);
      renderOrder();
    });
    row.appendChild(stepper); row.appendChild(add); row.appendChild(note);
    foot.appendChild(row);
    card.appendChild(foot);
    return card;
  }

  function renderCatalogue() {
    var root = document.querySelector("[data-catalogue]");
    if (!root) return;
    var keep = {};
    root.querySelectorAll("input[data-qty]").forEach(function (i) { keep[i.id] = i.value; });
    root.textContent = "";
    ["units", "packs", "accessories"].forEach(function (group) {
      var items = products.filter(function (p) { return p.group === group; });
      if (!items.length) return;
      root.appendChild(el("h2", { class: "group-title", id: "g-" + group }, T("store.group." + group)));
      items.forEach(function (p) { root.appendChild(productCard(p, keep)); });
    });
  }

  function renderOrder() {
    var cart = readCart();
    var lines = cartLines(cart);
    var list = document.querySelector("[data-lines]");
    if (!list) return;
    list.textContent = "";
    lines.forEach(function (l, idx) {
      var li = el("li");
      var nameId = "ln-" + l.id;
      li.appendChild(el("span", { class: "name", id: nameId }, l.name));
      li.appendChild(el("span", { class: "amt" }, money.format(l.lineTotal)));
      var p = byId[l.id];
      var label = p.perUnit ? T("store.units").replace("{n}", l.qty) + " × " + money.format(l.unitPrice)
        : l.qty + " × " + money.format(l.unitPrice) + (p.units ? " (" + T("store.units").replace("{n}", p.units) + ")" : "");
      var sub = el("span", { class: "sub" }, label + " ");
      var rmId = "rm-" + l.id;
      var rm = el("button", { type: "button", class: "remove", id: rmId, "aria-labelledby": rmId + " " + nameId }, T("store.remove"));
      rm.addEventListener("click", function () {
        var c = readCart();
        delete c[l.id];
        writeCart(c);
        renderOrder();
        // Keep keyboard focus in the order list instead of losing it to <body>.
        var btns = list.querySelectorAll("button.remove");
        var next = btns[idx] || btns[idx - 1];
        if (next) next.focus();
        else { var h = document.getElementById("order-title"); h.setAttribute("tabindex", "-1"); h.focus(); }
      });
      sub.appendChild(rm);
      li.appendChild(sub);
      list.appendChild(li);
    });
    document.querySelector("[data-empty]").hidden = lines.length > 0;
    document.querySelector("[data-total]").textContent = money.format(cartTotal(lines));
  }

  // ------------------------------------------------------------- form
  function normalisePhone(v) {
    var d = String(v || "").replace(/[^\d]/g, "");
    if (d.length === 12 && d.indexOf("91") === 0) d = d.slice(2);
    if (d.length === 11 && d.charAt(0) === "0") d = d.slice(1);
    return d;
  }
  function normalisePin(v) { return String(v || "").replace(/\s/g, ""); }

  var rules = {
    name: function (v) { return v.trim().length >= 2 ? null : "err.name"; },
    phone: function (v) { return /^[6-9]\d{9}$/.test(normalisePhone(v)) ? null : "err.phone"; },
    email: function (v) { return !v.trim() || /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(v.trim()) ? null : "err.email"; },
    address: function (v) { return v.trim().length >= 8 ? null : "err.address"; },
    city: function (v) { return v.trim().length >= 2 ? null : "err.city"; },
    pin: function (v) { return /^[1-9]\d{5}$/.test(normalisePin(v)) ? null : "err.pin"; },
    state: function (v) { return v.trim().length >= 2 ? null : "err.state"; },
  };

  /** Add or remove one id in aria-describedby, keeping any others (such as a hint). */
  function setDescribedBy(input, id, on) {
    var ids = (input.getAttribute("aria-describedby") || "").split(/\s+/).filter(function (x) { return x && x !== id; });
    if (on) ids.push(id);
    if (ids.length) input.setAttribute("aria-describedby", ids.join(" "));
    else input.removeAttribute("aria-describedby");
  }

  function setFieldError(input, key) {
    var field = input.closest(".field");
    var errId = input.id + "-err";
    var msg = field && field.querySelector(".error");
    if (key) {
      input.setAttribute("aria-invalid", "true");
      if (!msg && field) {
        msg = el("span", { class: "error", id: errId });
        field.appendChild(msg);
      }
      if (msg) { msg.textContent = T(key); setDescribedBy(input, errId, true); }
    } else {
      input.removeAttribute("aria-invalid");
      setDescribedBy(input, errId, false);
      if (msg) msg.remove();
    }
  }

  function validate(form) {
    var firstBad = null;
    Object.keys(rules).forEach(function (name) {
      var input = form.elements[name];
      if (!input) return;
      var key = rules[name](input.value, form);
      setFieldError(input, key);
      if (key && !firstBad) firstBad = input;
    });
    return firstBad;
  }

  function orderId() {
    var alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    var bytes = new Uint8Array(5);
    (window.crypto || window.msCrypto).getRandomValues(bytes);
    var tail = Array.prototype.map.call(bytes, function (b) { return alphabet.charAt(b % alphabet.length); }).join("");
    var d = new Date();
    var ymd = String(d.getFullYear()).slice(2) + String(d.getMonth() + 1).padStart(2, "0") + String(d.getDate()).padStart(2, "0");
    return "FM-" + ymd + "-" + tail;
  }

  // A retry of the same order keeps its ID, so the Sheet doesn't get it twice.
  var pending = { sig: null, id: null };

  function summaryText(order) {
    var rows = order.items.map(function (l) { return l.qty + " x " + l.name + (l.units && l.id !== "unit" ? " (" + l.units + " units)" : "") + " = " + money.format(l.lineTotal); });
    return [
      "FloodMesh pre-order " + order.id,
      rows.join("\n"),
      "Total: " + money.format(order.total),
      order.customer.name + ", " + order.customer.phone + (order.customer.email ? ", " + order.customer.email : ""),
      order.customer.org ? "Organisation: " + order.customer.org : "",
      order.customer.address + ", " + order.customer.city + " " + order.customer.pin + ", " + order.customer.state,
      order.customer.notes ? "Notes: " + order.customer.notes : "",
    ].filter(Boolean).join("\n");
  }

  function showResult(kind, order, errorKey) {
    var box = document.querySelector("[data-result]");
    var form = document.querySelector("[data-form]");
    box.textContent = "";
    var status = el("div", { class: "status " + (kind === "bad" ? "bad" : "ok") });
    if (kind === "sent") {
      status.appendChild(el("p", null, T("result.sent.title")));
      var id = el("p"); id.appendChild(el("code", null, order.id)); status.appendChild(id);
      status.appendChild(el("p", null, T("result.sent.body")));
    } else if (kind === "preview") {
      status.appendChild(el("p", null, T("result.preview.title")));
      var pid = el("p"); pid.appendChild(el("code", null, order.id)); status.appendChild(pid);
      status.appendChild(el("p", null, T("result.preview.body")));
    } else {
      var contact = [config.contactPhone, config.contactEmail].filter(Boolean).join(" / ");
      status.appendChild(el("p", null, T(errorKey || "result.failed").replace("{contact}", contact)));
    }
    box.appendChild(status);
    if (kind !== "sent") {
      var ta = el("textarea", { class: "summary-copy", readonly: "", "aria-label": T("result.summary") });
      ta.value = summaryText(order);
      box.appendChild(ta);
      var copy = el("button", { type: "button", class: "btn btn-line btn-small" }, T("result.copy"));
      copy.addEventListener("click", function () {
        var done = function () { copy.textContent = T("result.copied"); };
        var fallback = function () {
          var ok = false;
          try {
            ta.focus();
            ta.select();
            ta.setSelectionRange(0, ta.value.length);   // iOS needs this on read-only fields
            ok = document.execCommand("copy");
          } catch (e) { ok = false; }
          if (ok) done(); else copy.textContent = T("result.copy.manual");
        };
        if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(ta.value).then(done, fallback);
        else fallback();   // still inside the click, so execCommand has the user gesture
      });
      box.appendChild(copy);
    }
    // Show the box, then move focus to it: screen readers read the result, and
    // focus doesn't fall to <body> when the form is hidden after sending.
    box.hidden = false;
    box.focus({ preventScroll: true });
    if (kind === "sent") form.hidden = true;
    box.scrollIntoView({ behavior: "smooth", block: "nearest" });
  }

  function onSubmit(e) {
    e.preventDefault();
    var form = e.target;
    var errBox = document.querySelector("[data-form-error]");
    errBox.hidden = true;
    var lines = cartLines(readCart());
    if (!lines.length) { errBox.textContent = T("err.empty"); errBox.hidden = false; return; }
    var bad = validate(form);
    if (bad) { bad.focus(); return; }
    if (!form.elements.consent.checked) { errBox.textContent = T("err.consent"); errBox.hidden = false; form.elements.consent.focus(); return; }

    var order = {
      id: null,
      createdAt: new Date().toISOString(),
      lang: window.FM.lang(),
      items: lines,
      total: cartTotal(lines),
      preview: !!config.preview,
      customer: {
        name: form.elements.name.value.trim(),
        phone: normalisePhone(form.elements.phone.value),
        email: form.elements.email.value.trim(),
        org: form.elements.org.value.trim(),
        address: form.elements.address.value.trim(),
        city: form.elements.city.value.trim(),
        pin: normalisePin(form.elements.pin.value),
        state: form.elements.state.value.trim(),
        notes: form.elements.notes.value.trim(),
      },
      consent: true,
      page: window.location.pathname,
    };
    var sig = JSON.stringify([order.items, order.customer]);
    if (sig !== pending.sig) pending = { sig: sig, id: orderId() };
    order.id = pending.id;

    // The hidden trap field: people never see it, form-filling bots do.
    // Send nothing, but show the details so a real person can still reach us.
    if (form.elements.fm_hp_x && form.elements.fm_hp_x.value) { showResult("bad", order, "result.failed"); return; }

    if (!config.orderEndpoint) { showResult("preview", order); return; }

    var submit = form.querySelector("[data-submit]");
    submit.disabled = true;
    submit.setAttribute("aria-busy", "true");
    submit.textContent = T("form.sending");
    // text/plain keeps this a "simple" request with no CORS preflight.
    fetch(config.orderEndpoint, { method: "POST", headers: { "Content-Type": "text/plain;charset=utf-8" }, body: JSON.stringify(order) })
      .then(function (r) { return r.json(); })
      .then(function (res) {
        if (res && res.ok) {
          pending = { sig: null, id: null };
          writeCart({});
          renderOrder();
          showResult("sent", order);
        } else {
          showResult("bad", order, res && res.error === "rate" ? "result.rate" : "result.failed");
        }
      })
      .catch(function () { showResult("bad", order, "result.failed"); })
      .then(function () { submit.disabled = false; submit.removeAttribute("aria-busy"); submit.textContent = T("form.submit"); });
  }

  document.addEventListener("DOMContentLoaded", function () {
    // Save the cleaned cart back, so the header count matches the order panel.
    var raw = {};
    try { raw = JSON.parse(window.FM.store("fm-cart") || "{}") || {}; } catch (e) {}
    var clean = readCart();
    if (JSON.stringify(clean) !== JSON.stringify(raw)) writeCart(clean);

    renderCatalogue();
    renderOrder();
    var form = document.querySelector("[data-form]");
    if (form) {
      form.addEventListener("submit", onSubmit);
      // Clear an error as soon as the field is corrected. New errors appear
      // only on Send: adding or removing them on blur shifts the layout under
      // a pointer on its way to the Send button, and the click is lost.
      form.addEventListener("input", function (e) {
        var t = e.target, name = t && t.name;
        if (rules[name] && t.getAttribute("aria-invalid") === "true") setFieldError(t, rules[name](t.value, form));
      });
      form.hidden = false;   // shown only once its handler is attached
    }
    document.addEventListener("fm:lang", function () {
      renderCatalogue();
      renderOrder();
      var s = document.querySelector("[data-submit]");
      if (s && s.disabled) s.textContent = T("form.sending");   // applyLang reset it
    });
    window.addEventListener("storage", function (e) { if (e.key === "fm-cart") renderOrder(); });
  });
})();
