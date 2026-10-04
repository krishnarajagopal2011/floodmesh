/* FloodMesh store: catalogue, cart (kept in this browser), and the pre-order
   form. Orders go to FM_CONFIG.orderEndpoint (a Google Apps Script web app
   that appends a row to the pre-orders Sheet). No payment is taken. */
(function () {
  "use strict";

  var T = function (key) { return window.FM.t(key); };
  var products = window.FM_PRODUCTS || [];
  var config = window.FM_CONFIG || {};
  var byId = {};
  products.forEach(function (p) { byId[p.id] = p; });

  var money = new Intl.NumberFormat("en-IN", { style: "currency", currency: "INR", maximumFractionDigits: 0 });

  // ------------------------------------------------------------- cart
  function readCart() {
    var raw = window.FM.readCart();
    var cart = {};
    Object.keys(raw).forEach(function (id) {
      var p = byId[id];
      var q = Math.floor(Number(raw[id]));
      if (p && q > 0) cart[id] = Math.min(q, p.maxQty);
    });
    return cart;
  }
  function writeCart(cart) {
    window.FM.store("fm-cart", JSON.stringify(cart));
    document.dispatchEvent(new CustomEvent("fm:cart"));
  }
  function cartLines(cart) {
    return products.filter(function (p) { return cart[p.id]; }).map(function (p) {
      return { id: p.id, name: T("p." + p.id + ".name"), qty: cart[p.id], unitPrice: p.price, lineTotal: p.price * cart[p.id] };
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

  function renderCatalogue() {
    var root = document.querySelector("[data-catalogue]");
    if (!root) return;
    var keep = {};
    root.querySelectorAll("input[data-qty]").forEach(function (i) { keep[i.id] = i.value; });
    root.textContent = "";
    products.forEach(function (p) {
      var card = el("article", { class: "product", id: p.id, "aria-labelledby": "t-" + p.id });

      var media = el("figure", { class: "media-slot", "data-asset": p.media });
      media.appendChild(el("span", { class: "tag" }, T("media.product")));
      card.appendChild(media);

      var body = el("div", { class: "product-body" });
      var badge = T("p." + p.id + ".badge");
      if (badge && badge !== "p." + p.id + ".badge") body.appendChild(el("span", { class: "badge" }, badge));
      body.appendChild(el("h2", { id: "t-" + p.id }, T("p." + p.id + ".name")));
      body.appendChild(el("p", { class: "tagline" }, T("p." + p.id + ".tagline")));
      var ul = el("ul");
      for (var b = 1; b <= p.bullets; b++) ul.appendChild(el("li", null, T("p." + p.id + ".b" + b)));
      body.appendChild(ul);
      card.appendChild(body);

      var foot = el("div", { class: "product-foot" });
      var price = el("p", { class: "price" }, money.format(p.price));
      var small = [];
      if (p.bundle) {
        var separate = Object.keys(p.bundle).reduce(function (s, id) { return s + byId[id].price * p.bundle[id]; }, 0);
        small.push(T("store.separately").replace("{amount}", money.format(separate)));
      } else {
        small.push(T("store.perunit"));
      }
      if (config.preview) small.push(T("store.placeholder"));
      price.appendChild(el("small", null, small.join(" · ")));
      foot.appendChild(price);

      var row = el("div", { class: "add-row" });
      var stepper = el("div", { class: "stepper" });
      var qid = "q-" + p.id;
      var minus = el("button", { type: "button", "aria-label": T("store.less") }, "−");
      var input = el("input", { id: qid, "data-qty": p.id, type: "number", inputmode: "numeric", min: "1", max: String(p.maxQty), value: keep[qid] || "1", "aria-label": T("store.qty") });
      var plus = el("button", { type: "button", "aria-label": T("store.more") }, "+");
      minus.addEventListener("click", function () { input.value = String(Math.max(1, (parseInt(input.value, 10) || 1) - 1)); });
      plus.addEventListener("click", function () { input.value = String(Math.min(p.maxQty, (parseInt(input.value, 10) || 0) + 1)); });
      stepper.appendChild(minus); stepper.appendChild(input); stepper.appendChild(plus);
      var add = el("button", { type: "button", class: "btn btn-line btn-small" }, T("store.add"));
      add.addEventListener("click", function () {
        var q = Math.max(1, Math.min(p.maxQty, parseInt(input.value, 10) || 1));
        var cart = readCart();
        cart[p.id] = Math.min(p.maxQty, (cart[p.id] || 0) + q);
        writeCart(cart);
        input.value = "1";
        add.textContent = T("store.added");
        window.setTimeout(function () { add.textContent = T("store.add"); }, 1600);
        renderOrder();
      });
      row.appendChild(stepper); row.appendChild(add);
      foot.appendChild(row);
      card.appendChild(foot);
      root.appendChild(card);
    });
  }

  function renderOrder() {
    var cart = readCart();
    var lines = cartLines(cart);
    var list = document.querySelector("[data-lines]");
    if (!list) return;
    list.textContent = "";
    lines.forEach(function (l) {
      var li = el("li");
      li.appendChild(el("span", { class: "name" }, l.name));
      li.appendChild(el("span", { class: "amt" }, money.format(l.lineTotal)));
      var sub = el("span", { class: "sub" }, l.qty + " × " + money.format(l.unitPrice) + " ");
      var rm = el("button", { type: "button", class: "remove" }, T("store.remove"));
      rm.addEventListener("click", function () {
        var c = readCart();
        delete c[l.id];
        writeCart(c);
        renderOrder();
      });
      sub.appendChild(rm);
      li.appendChild(sub);
      list.appendChild(li);
    });
    document.querySelector("[data-empty]").hidden = lines.length > 0;
    document.querySelector("[data-total]").textContent = money.format(cartTotal(lines));
    var needsOrg = lines.some(function (l) { return byId[l.id].orgOnly; });
    var org = document.querySelector("[data-org-field]");
    org.hidden = !needsOrg;
    document.getElementById("f-org").required = needsOrg;
  }

  // ------------------------------------------------------------- form
  function normalisePhone(v) {
    var d = String(v || "").replace(/[^\d]/g, "");
    if (d.length === 12 && d.indexOf("91") === 0) d = d.slice(2);
    if (d.length === 11 && d.charAt(0) === "0") d = d.slice(1);
    return d;
  }

  var rules = {
    name: function (v) { return v.trim().length >= 2 ? null : "err.name"; },
    phone: function (v) { return /^[6-9]\d{9}$/.test(normalisePhone(v)) ? null : "err.phone"; },
    email: function (v) { return !v.trim() || /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(v.trim()) ? null : "err.email"; },
    org: function (v, form) { return document.getElementById("f-org").required && v.trim().length < 2 ? "err.org" : null; },
    address: function (v) { return v.trim().length >= 8 ? null : "err.address"; },
    city: function (v) { return v.trim().length >= 2 ? null : "err.city"; },
    pin: function (v) { return /^[1-9]\d{5}$/.test(v.trim()) ? null : "err.pin"; },
    state: function (v) { return v.trim().length >= 2 ? null : "err.state"; },
  };

  function setFieldError(input, key) {
    var field = input.closest(".field");
    var msg = field && field.querySelector(".error");
    if (key) {
      input.setAttribute("aria-invalid", "true");
      if (!msg && field) {
        msg = el("span", { class: "error", id: input.id + "-err" });
        field.appendChild(msg);
      }
      if (msg) { msg.textContent = T(key); input.setAttribute("aria-describedby", msg.id); }
    } else {
      input.removeAttribute("aria-invalid");
      if (msg) msg.remove();
      input.removeAttribute("aria-describedby");
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

  function summaryText(order) {
    var rows = order.items.map(function (l) { return l.qty + " x " + l.name + " = " + money.format(l.lineTotal); });
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
    var status = el("div", { class: "status " + (kind === "bad" ? "bad" : "ok"), role: "status" });
    if (kind === "sent") {
      status.appendChild(el("p", null, T("result.sent.title")));
      var id = el("p"); id.appendChild(el("code", null, order.id)); status.appendChild(id);
      status.appendChild(el("p", null, T("result.sent.body")));
    } else if (kind === "preview") {
      status.appendChild(el("p", null, T("result.preview.title")));
      var pid = el("p"); pid.appendChild(el("code", null, order.id)); status.appendChild(pid);
      status.appendChild(el("p", null, T("result.preview.body")));
    } else {
      status.appendChild(el("p", null, T(errorKey || "result.failed")));
    }
    box.appendChild(status);
    if (kind !== "sent") {
      var ta = el("textarea", { class: "summary-copy", readonly: "", "aria-label": T("result.summary") });
      ta.value = summaryText(order);
      box.appendChild(ta);
      var copy = el("button", { type: "button", class: "btn btn-line btn-small" }, T("result.copy"));
      copy.addEventListener("click", function () {
        var done = function () { copy.textContent = T("result.copied"); };
        try {
          navigator.clipboard.writeText(ta.value).then(done, function () { ta.select(); });
        } catch (e) { ta.select(); }
      });
      box.appendChild(copy);
    }
    if (kind === "sent") form.hidden = true;
    box.hidden = false;
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
    if (form.elements.website.value) return;   // a bot filled the hidden field

    var order = {
      id: orderId(),
      createdAt: new Date().toISOString(),
      lang: document.documentElement.lang,
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
        pin: form.elements.pin.value.trim(),
        state: form.elements.state.value.trim(),
        notes: form.elements.notes.value.trim(),
      },
      consent: true,
      page: window.location.href,
    };

    if (!config.orderEndpoint) { showResult("preview", order); return; }

    var submit = form.querySelector("[data-submit]");
    submit.disabled = true;
    submit.textContent = T("form.sending");
    // text/plain keeps this a "simple" request, which Apps Script accepts without a CORS preflight.
    fetch(config.orderEndpoint, { method: "POST", headers: { "Content-Type": "text/plain;charset=utf-8" }, body: JSON.stringify(order) })
      .then(function (r) { return r.json(); })
      .then(function (res) {
        if (res && res.ok) {
          writeCart({});
          renderOrder();
          showResult("sent", order);
        } else {
          showResult("bad", order, "result.failed");
        }
      })
      .catch(function () { showResult("bad", order, "result.failed"); })
      .then(function () { submit.disabled = false; submit.textContent = T("form.submit"); });
  }

  document.addEventListener("DOMContentLoaded", function () {
    renderCatalogue();
    renderOrder();
    var form = document.querySelector("[data-form]");
    if (form) {
      form.addEventListener("submit", onSubmit);
      form.addEventListener("focusout", function (e) {
        var name = e.target && e.target.name;
        if (rules[name] && e.target.value) setFieldError(e.target, rules[name](e.target.value, form));
      });
    }
    document.addEventListener("fm:lang", function () { renderCatalogue(); renderOrder(); });
    window.addEventListener("storage", function (e) { if (e.key === "fm-cart") renderOrder(); });
  });
})();
