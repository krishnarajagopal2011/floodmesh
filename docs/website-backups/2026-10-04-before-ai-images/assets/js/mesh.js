/* FloodMesh website: the animated street map in "How it works".
   Two scenes on the same street:
   - Message: a neighbour's short message wakes the units in reach, shows on
     every one of them, and is passed on (a plugged-in rooftop unit reaches
     further).
   - SOS: an SOS hops quietly to a rescue team's responder unit, and
     DELIVERED comes back.
   It is an illustration of what a buyer sees, not a radio simulation. In
   each wave only the units needed to reach new homes pass it on, as on the
   real units. Respects prefers-reduced-motion and pauses when off screen. */
(function () {
  "use strict";

  var SVGNS = "http://www.w3.org/2000/svg";
  var T = function (k) { return window.FM ? window.FM.t(k) : k; };

  // Street layout (viewBox 800 x 400). Ranges are illustrative.
  var NODES = [
    { kind: "home", x: 70, y: 300 }, { kind: "home", x: 150, y: 225 }, { kind: "home", x: 175, y: 330 },
    { kind: "home", x: 255, y: 250 }, { kind: "home", x: 300, y: 345 }, { kind: "roof", x: 345, y: 190 },
    { kind: "home", x: 440, y: 300 }, { kind: "home", x: 470, y: 150 }, { kind: "home", x: 545, y: 245 },
    { kind: "home", x: 600, y: 340 }, { kind: "home", x: 630, y: 195 }, { kind: "resp", x: 720, y: 270 },
  ];
  var RANGE = { home: 115, roof: 230, resp: 140 };
  var MSG_FROM = 4, SOS_FROM = 0, RESP = 11;

  var adj = NODES.map(function () { return []; });
  NODES.forEach(function (a, i) {
    NODES.forEach(function (b, j) {
      if (i < j && Math.hypot(a.x - b.x, a.y - b.y) <= Math.max(RANGE[a.kind], RANGE[b.kind])) { adj[i].push(j); adj[j].push(i); }
    });
  });
  function bfs(src) {
    var layer = {}, parent = {}, q = [src];
    layer[src] = 0; parent[src] = -1;
    for (var k = 0; k < q.length; k++) {
      adj[q[k]].forEach(function (w) { if (!(w in layer)) { layer[w] = layer[q[k]] + 1; parent[w] = q[k]; q.push(w); } });
    }
    return { layer: layer, parent: parent };
  }

  function el(name, attrs, parent) {
    var n = document.createElementNS(SVGNS, name);
    Object.keys(attrs || {}).forEach(function (k) { n.setAttribute(k, attrs[k]); });
    if (parent) parent.appendChild(n);
    return n;
  }

  function init(fig) {
    var svg = fig.querySelector("svg");
    var caption = fig.querySelector("[data-mesh-caption]");
    var btnMsg = fig.querySelector('[data-mesh-scene="msg"]');
    var btnSos = fig.querySelector('[data-mesh-scene="sos"]');
    var btnPause = fig.querySelector("[data-mesh-pause]");
    var reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

    // ---- static map
    var base = el("g", { class: "mesh-base" }, svg);
    el("rect", { x: 0, y: 330, width: 800, height: 70, class: "mesh-water" }, base);
    el("path", { d: "M0 282 C120 262 210 302 320 288 S520 236 620 282 S760 262 800 258", class: "mesh-road" }, base);
    el("path", { d: "M300 400 C320 300 360 240 380 120 M560 400 C560 330 520 260 540 110", class: "mesh-road thin" }, base);
    var linkLayer = el("g", {}, svg);
    var ringLayer = el("g", {}, svg);
    var nodeLayer = el("g", {}, svg);
    var labelLayer = el("g", { class: "mesh-labels" }, svg);

    var nodes = NODES.map(function (n, i) {
      var g = el("g", { class: "mesh-node " + n.kind, transform: "translate(" + n.x + " " + n.y + ")" }, nodeLayer);
      if (n.kind === "resp") {
        el("path", { d: "M-24 4 L24 4 L16 16 L-16 16 Z", class: "hull" }, g);          // a boat
        el("rect", { x: -8, y: -12, width: 16, height: 16, rx: 2, class: "house" }, g);
      } else {
        el("path", { d: "M-17 -6 L0 -21 L17 -6 Z", class: "roof-shape" }, g);
        el("rect", { x: -15, y: -7, width: 30, height: 24, rx: 2, class: "house" }, g);
        if (n.kind === "roof") el("path", { d: "M10 -14 L10 -40 M4 -40 L16 -40", class: "antenna" }, g);
      }
      el("circle", { cx: 0, cy: n.kind === "resp" ? -4 : 5, r: 6, class: "dot" }, g);
      var bubble = el("g", { class: "bubble", transform: "translate(" + (n.kind === "resp" ? -14 : -12) + " -52)" }, g);
      el("rect", { x: 0, y: 0, width: 24, height: 16, rx: 3 }, bubble);
      el("path", { d: "M8 16 L12 22 L16 16 Z" }, bubble);
      var bt = el("text", { x: 12, y: 12, "text-anchor": "middle" }, bubble);
      return { g: g, bubble: bubble, bt: bt, data: n, index: i };
    });

    // On a narrow screen the whole map is drawn small; enlarge the houses and
    // units (not the distances) so they stay readable.
    function fitNodes() {
      var k = svg.getBoundingClientRect().width < 520 ? 1.7 : 1;
      nodes.forEach(function (n) {
        n.g.setAttribute("transform", "translate(" + n.data.x + " " + n.data.y + ")" + (k === 1 ? "" : " scale(" + k + ")"));
      });
    }
    fitNodes();
    if ("ResizeObserver" in window) new ResizeObserver(fitNodes).observe(svg);

    function labelAt(x, y, key, cls) {
      var t = el("text", { x: x, y: y, class: "mesh-label " + (cls || ""), "text-anchor": "middle", "data-key": key }, labelLayer);
      t.textContent = T(key);
      return t;
    }
    var fixedLabels = [labelAt(345, 132, "anim.label.roof"), labelAt(720, 318, "anim.label.resp")];
    var screenLabel = el("text", { x: 24, y: 372, class: "mesh-label screen", "text-anchor": "start" }, labelLayer);

    // ---- node state
    function setState(i, state, bubbleText) {
      var n = nodes[i];
      n.g.setAttribute("class", "mesh-node " + n.data.kind + " " + state);
      n.bt.textContent = bubbleText || "";
    }
    function resetStates() {
      nodes.forEach(function (n) {
        // Plugged-in and responder units are always awake; battery units sleep.
        setState(n.index, n.data.kind === "home" ? "asleep" : "awake");
      });
      screenLabel.textContent = "";
      while (ringLayer.firstChild) ringLayer.removeChild(ringLayer.firstChild);
      while (linkLayer.firstChild) linkLayer.removeChild(linkLayer.firstChild);
    }

    // ---- tiny animation engine
    var tweens = [], events = [], t0 = 0, now = 0, running = false, paused = false, raf = 0, scene = "msg";
    function ring(i, color, dur) {
      var n = NODES[i];
      var c = el("circle", { cx: n.x, cy: n.y, r: 4, class: "ring " + color }, ringLayer);
      tweens.push({ start: now, dur: dur || 900, step: function (p) {
        c.setAttribute("r", 4 + (RANGE[n.kind] - 4) * p);
        c.setAttribute("opacity", String(0.9 * (1 - p)));
        if (p >= 1) c.remove();
      } });
    }
    /** A hop: a line that grows from one unit to the next and stays as a trail. */
    function link(from, to, color, dur) {
      var a = NODES[from], b = NODES[to];
      var len = Math.hypot(b.x - a.x, b.y - a.y);
      var line = el("line", { x1: a.x, y1: a.y, x2: b.x, y2: b.y, class: "hop " + color,
        "stroke-dasharray": len, "stroke-dashoffset": len }, linkLayer);
      if (reduce) { line.setAttribute("stroke-dashoffset", "0"); return; }
      tweens.push({ start: now, dur: dur || 700, step: function (p) {
        line.setAttribute("stroke-dashoffset", String(len * (1 - p)));
      } });
    }
    function at(ms, fn) { events.push({ t: ms, fn: fn }); }
    function say(key) { caption.textContent = T(key); caption.setAttribute("data-key", key); }

    function buildMessage() {
      var r = bfs(MSG_FROM), maxL = 0;
      Object.keys(r.layer).forEach(function (k) { maxL = Math.max(maxL, r.layer[k]); });
      at(0, function () { say("anim.m1"); setState(MSG_FROM, "shown", "…"); });
      var gap = 1500;
      for (var L = 0; L < maxL; L++) (function (L) {
        // Only units that reach someone new pass it on.
        var senders = Object.keys(r.parent).filter(function (k) {
          return r.layer[k] === L && Object.keys(r.parent).some(function (c) { return String(r.parent[c]) === k; });
        }).map(Number);
        at(600 + L * gap, function () {
          if (L === 1) say("anim.m3");
          senders.forEach(function (s) {
            ring(s, "msg");
            Object.keys(r.parent).forEach(function (c) { if (r.parent[c] === s) link(s, Number(c), "msg"); });
          });
        });
        at(600 + L * gap + 750, function () {
          if (L === 0) say("anim.m2");
          Object.keys(r.layer).forEach(function (k) { if (r.layer[k] === L + 1) setState(Number(k), "shown", "…"); });
        });
      })(L);
      at(600 + maxL * gap + 300, function () { say("anim.m4"); });
      return 600 + maxL * gap + 3600;
    }

    function buildSos() {
      var r = bfs(SOS_FROM), path = [RESP];
      while (r.parent[path[path.length - 1]] !== -1) path.push(r.parent[path[path.length - 1]]);
      path.reverse();
      var gap = 1100, t = 600;
      at(0, function () { say("anim.s1"); setState(SOS_FROM, "sos", "SOS"); });
      path.slice(0, -1).forEach(function (from, i) {
        var to = path[i + 1];
        at(t + i * gap, function () { if (i === 0) say("anim.s2"); ring(from, "sos"); link(from, to, "sos"); });
        at(t + i * gap + 700, function () {
          if (to === RESP) { setState(to, "alert", "SOS"); say("anim.s3"); }
          else setState(to, "quiet");   // passes it on, shows nothing
        });
      });
      var back = t + (path.length - 1) * gap + 900;
      var rev = path.slice().reverse();
      rev.slice(0, -1).forEach(function (from, i) {
        at(back + i * 500, function () { if (i === 0) say("anim.s4"); ring(from, "ack", 600); link(from, rev[i + 1], "ack", 450); });
      });
      var arrive = back + (path.length - 1) * 500 + 300;
      at(arrive, function () { screenLabel.textContent = "DELIVERED"; setState(SOS_FROM, "sos done", "✓"); });
      at(arrive + 1600, function () { say("anim.s5"); screenLabel.textContent = "HELP IS COMING"; });
      return arrive + 4600;
    }

    var length = 0;
    function startScene(name) {
      scene = name;
      btnMsg.setAttribute("aria-pressed", String(name === "msg"));
      btnSos.setAttribute("aria-pressed", String(name === "sos"));
      tweens = []; events = [];
      resetStates();
      length = name === "msg" ? buildMessage() : buildSos();
      events.sort(function (a, b) { return a.t - b.t; });
      if (reduce) {
        // No motion: jump to the scene's last frame.
        now = 0;
        events.forEach(function (e) { e.fn(); });
        while (ringLayer.firstChild) ringLayer.removeChild(ringLayer.firstChild);
        tweens = [];
        events = [];
        return;
      }
      t0 = performance.now(); now = 0;
    }

    function frame(ts) {
      raf = 0;
      if (!running || paused) return;
      now = ts - t0;
      while (events.length && events[0].t <= now) events.shift().fn();
      tweens = tweens.filter(function (tw) {
        var p = Math.min(1, (now - tw.start) / tw.dur);
        tw.step(p);
        return p < 1;
      });
      if (now > length) startScene(scene === "msg" ? "sos" : "msg");
      raf = requestAnimationFrame(frame);
    }
    function play() { if (!raf && running && !paused && !reduce) raf = requestAnimationFrame(frame); }

    btnMsg.addEventListener("click", function () { startScene("msg"); play(); });
    btnSos.addEventListener("click", function () { startScene("sos"); play(); });
    if (reduce) btnPause.hidden = true;
    btnPause.addEventListener("click", function () {
      paused = !paused;
      btnPause.setAttribute("aria-pressed", String(paused));
      btnPause.textContent = T(paused ? "anim.play" : "anim.pause");
      btnPause.setAttribute("data-i18n", paused ? "anim.play" : "anim.pause");
      if (!paused) { t0 = performance.now() - now; play(); }
    });

    // Run only while the map is on screen.
    if ("IntersectionObserver" in window) {
      new IntersectionObserver(function (entries) {
        running = entries[0].isIntersecting;
        if (running) { t0 = performance.now() - now; play(); }
      }).observe(fig);
    } else { running = true; }

    document.addEventListener("fm:lang", function () {
      fixedLabels.forEach(function (l) { l.textContent = T(l.getAttribute("data-key")); });
      var k = caption.getAttribute("data-key"); if (k) caption.textContent = T(k);
    });

    startScene("msg");
    play();
  }

  document.addEventListener("DOMContentLoaded", function () {
    document.querySelectorAll("[data-mesh]").forEach(init);
  });
})();
