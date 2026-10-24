// RDMA Display client UI: app tiles to start a stream, plain-language settings that save on change.
// Talks only to rdx-clientd (/api/state once a second, /api/config, /api/session/*, /api/icon).
"use strict";

const $ = (s) => document.querySelector(s);
const form = $("#settings");
let state = null;
let shownTiles = "";         // JSON of what the tiles show (no timers), to rebuild them only on change
let shownPaused = "";        // JSON of the paused-app bands, rebuilt only on change
let shownSettings = "";      // JSON of what the settings form shows, to refill only on change
let saveTimer = null;
let logProc = null;

async function api(path, body) {
  const opt = body === undefined ? {} : {
    method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(body),
  };
  const r = await fetch(path, opt);
  const j = await r.json().catch(() => ({ok: false, error: "bad reply"}));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}

function el(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (v !== false && v !== null && v !== undefined) e.setAttribute(k, v === true ? "" : v);
  }
  for (const k of kids) if (k !== null && k !== undefined) e.append(k instanceof Node ? k : document.createTextNode(k));
  return e;
}

function svg(path) {
  const s = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  s.setAttribute("viewBox", "0 0 48 48");
  s.setAttribute("fill", "none");
  s.setAttribute("stroke", "currentColor");
  s.setAttribute("stroke-width", "3");
  s.setAttribute("stroke-linecap", "round");
  s.setAttribute("stroke-linejoin", "round");
  s.innerHTML = path;
  return s;
}
const ICON_DESKTOP = '<rect x="5" y="8" width="38" height="24" rx="3"/><path d="M17 40h14M24 32v8"/>';
const ICON_GAME = '<path d="M14 16h20a9 9 0 0 1 9 9v2a7 7 0 0 1-12.5 4.3L28 28h-8l-2.5 3.3A7 7 0 0 1 5 27v-2a9 9 0 0 1 9-9z"/><path d="M14 21v8M10 25h8"/><circle cx="32" cy="23" r="1.5"/><circle cx="36" cy="27" r="1.5"/>';
const ICON_APP = '<rect x="8" y="8" width="13" height="13" rx="2"/><rect x="27" y="8" width="13" height="13" rx="2"/><rect x="8" y="27" width="13" height="13" rx="2"/><rect x="27" y="27" width="13" height="13" rx="2"/>';

const fmtMode = (w, h, hz) => `${w} × ${h}` + (hz ? ` · ${hz} Hz` : "");

/* ---------------- what an app looks like ---------------- */

function outputsOf(st) {
  const info = st.host.info;
  return Object.fromEntries(((info && info.outputs) || []).map((o) => [o.name, o]));
}

function appView(a, outs) {
  const out = outs[a.output || a.id.replace(/^mirror:/, "")];
  if (a.kind === "mirror") {
    const title = /^Mirror /.test(a.name) ? `Desktop ${a.output || ""}`.trim() : a.name;
    return {title, sub: out ? `Whole screen · ${fmtMode(out.w, out.h, out.hz)}` : "Whole screen", icon: ICON_DESKTOP};
  }
  const game = /steam|game|proton|wine/i.test(`${a.name} ${a.id}`);
  return {title: a.name, sub: game ? "Game" : "App", icon: game ? ICON_GAME : ICON_APP};
}

function tileIcon(a, view, host) {
  if (!a.icon) return svg(view.icon);
  const q = host === undefined ? "" : `&host=${host}`;
  const img = el("img", {src: `/api/icon?app=${encodeURIComponent(a.id)}${q}`, alt: ""});
  img.addEventListener("error", () => img.replaceWith(svg(view.icon)));
  return img;
}

/* ---------------- home ---------------- */

function renderHome(st) {
  const h = st.host, s = st.session;
  const pill = $("#host-pill");
  if (h.connected && h.info) {
    const act = (st.hosts || []).find((x) => x.active);
    pill.textContent = `Connected to ${act && act.name ? act.name : h.info.host_name || h.address}`;
    pill.className = "pill ok";
  } else {
    pill.textContent = "Not connected";
    pill.className = "pill bad";
  }

  const notice = $("#notice");
  let msg = "";
  if (!h.connected) {
    msg = `Can't reach the other computer (${h.address}). Check that “RDMA Display Host” is running there ` +
          "and that the cable between the two computers is plugged in.";
    notice.title = h.error || "";
  } else if (s.error && s.state === "idle") {
    msg = `The last stream ended: ${s.error}`;
    notice.title = "";
  }
  notice.hidden = !msg;
  notice.textContent = msg;

  const outs = outputsOf(st);
  const apps = (h.info && h.info.apps) || [];
  const byId = Object.fromEntries(apps.map((a) => [a.id, a]));
  const busy = s.state !== "idle";

  const run = $("#running");
  run.hidden = !busy;
  if (busy) {
    const a = byId[s.app];
    $("#now-app").textContent = a ? appView(a, outs).title : s.app || "";
    $("#now-label").textContent = s.state === "running" ? "Streaming" : s.state === "stopping" ? "Stopping …" : "Starting …";
    const m = s.mode !== undefined ? s.mode : st.config.mode;    /* a resumed app keeps its own mode */
    const parts = [];
    if (s.resumed) parts.push("resumed");
    if (s.state === "running" && s.since_s !== undefined) parts.push(`for ${fmtDuration(s.since_s)}`);
    const out = a && a.kind === "mirror" ? outs[a.output || a.id.replace(/^mirror:/, "")] : null;
    if (st.config.tearing) parts.push("tearing allowed");
    if (m && out && st.config.match_host && !st.config.tearing) parts.push(fmtMode(m.w, m.h, Math.round((out.hz_exact || out.hz) * 100) / 100) + " (matched)");
    else if (m) parts.push(fmtMode(m.w, m.h, m.hz));
    /* variable refresh as the session runs it (clientd "display") */
    const d = st.display;
    if (d && d.vrr) parts.push(d.min_hz > 0 ? `FreeSync on (${Math.round(d.min_hz)}–${Math.round(d.max_hz)} Hz)` : "FreeSync on");
    $("#now-sub").textContent = parts.join(" · ");
    $("#stop").disabled = s.state === "stopping";
    /* an app keeps running on the other computer when we leave it; a mirrored desktop just stops */
    const isApp = a && a.kind !== "mirror";
    $("#stop").textContent = isApp ? "Disconnect" : "Stop";
    $("#stop").title = isApp ? "Leave the app running on the other computer; resume it later from its tile" : "";
  }

  /* every app still running on the other computer (several with a host that keeps them, "suspend_multi") */
  const susList = suspendedOf(h.info, h).filter((x) => byId[x.app] && !(busy && s.app === x.app));
  const susById = Object.fromEntries(susList.map((x) => [x.app, x]));
  const multi = !!(h.suspend_multi || (h.info && h.info.suspend_multi));
  /* little free graphics memory when this stream started: which apps hold it (nothing was closed) */
  const vw = $("#vram-warn"), w = busy && s.vram_warning;
  vw.hidden = !w;
  if (w) vw.textContent = `Little graphics memory left on the other computer: ${w.free_mib} MiB free` +
    (w.apps && w.apps.length ? ` · still running: ${w.apps.map((x) => `${x.name || x.app} (${x.vram_mib} MiB)`).join(", ")}` : "") +
    ". Quit one below if the game stutters or does not start.";
  /* each paused app gets its own band with Resume and Quit, not only the small tile button */
  const pausedSig = JSON.stringify([susList.map((x) => [x.app, x.mode, Math.floor((x.since_s || 0) / 60), x.vram_mib]), busy]);
  if (pausedSig !== shownPaused) {
    shownPaused = pausedSig;
    $("#paused-list").replaceChildren(...susList.map((x) => {
      const title = appTitle(x.app, byId, outs);
      const sub = [x.mode ? fmtMode(x.mode.w, x.mode.h, x.mode.hz) : "",
                   x.since_s !== undefined ? `paused for ${fmtDuration(x.since_s)}` : "",
                   x.vram_mib !== undefined ? `${x.vram_mib} MiB graphics memory` : ""].filter(Boolean).join(" · ");
      return el("section", {class: "now paused"},
        el("div", {}, el("p", {class: "now-label"}, busy ? "Also running on the other computer" : "Still running on the other computer"),
           el("h2", {}, title), el("p", {class: "muted"}, sub)),
        el("div", {class: "now-actions"},
           el("button", {class: "big", type: "button", disabled: busy, title: busy ? "Disconnect from the current stream first" : "",
                         onclick: () => start(x.app, null)}, "Resume"),
           el("button", {class: "big danger", type: "button", onclick: () => quitApp(x.app, title)}, "Quit app")));
    }));
  }
  const box = $("#apps");
  /* the page polls every second: rebuild the tiles (and reload their icons) only when what they
   * show changes, otherwise they flicker and the hover/focus state is lost */
  const tileSig = JSON.stringify([apps, outs, busy, s.app, susList.map((x) => [x.app, x.mode]), multi]);
  if (tileSig === shownTiles) return;
  shownTiles = tileSig;
  box.replaceChildren(...apps.map((a) => {
    const v = appView(a, outs);
    const sus = susById[a.id];
    const isSus = !!sus;
    const sm = isSus && sus.mode;
    /* a host with one app in the background closes it when another starts: ask first */
    const closes = !multi && !isSus && a.kind !== "mirror" && susList.length ? appTitle(susList[0].app, byId, outs) : null;
    const tile = el("button", {
      class: "tile" + (busy && s.app === a.id ? " active" : "") + (isSus ? " suspended" : ""), type: "button", disabled: busy,
      title: busy ? "Disconnect from the current stream first" : isSus ? `Resume ${v.title}` : `Start ${v.title}`,
      onclick: () => start(a.id, closes),
    }, tileIcon(a, v), el("b", {}, v.title),
       isSus ? el("span", {class: "run-badge"}, "Running · Resume") : null,
       el("small", {}, isSus && sm ? fmtMode(sm.w, sm.h, sm.hz) : v.sub));
    if (!isSus) return el("div", {class: "tilewrap"}, tile);
    const quit = el("button", {class: "quit", type: "button", disabled: busy, title: `Quit ${v.title} on the other computer`,
      onclick: (ev) => { ev.stopPropagation(); quitApp(a.id, v.title); }}, "Quit");
    return el("div", {class: "tilewrap"}, tile, quit);
  }));
  $("#apps-note").textContent = !h.connected ? "" : apps.length ? (busy ? "" : "Click what you want to see.")
    : "The other computer offers nothing to stream yet. Add an app in “RDMA Display Host” there.";
}

/* ---------------- network link (state.link, state.peer_link, state.link_warning) ---------------- */

const LEVEL_WORD = {ok: "OK", warn: "Warning", alarm: "Alarm", down: "Link down", failed: "Card failed", unknown: "Unknown"};
const LEVEL_BAD = ["warn", "alarm", "down", "failed"];

function fmtSecs(n) {
  n = Math.round(n);
  if (n < 60) return `${n} s`;
  if (n < 3600) return `${Math.floor(n / 60)} min` + (n % 60 ? ` ${n % 60} s` : "");
  return `${Math.floor(n / 3600)} h ${Math.floor((n % 3600) / 60)} min`;
}

function fmtClock(unix) {
  const d = new Date(unix * 1000);
  return Date.now() - d.getTime() > 12 * 3600 * 1000 ? d.toLocaleString() : d.toLocaleTimeString();
}

function tempClass(m) {
  if (m.alarm_c != null && m.temp_c >= m.alarm_c) return "alarm";
  if (m.warn_c != null && m.temp_c >= m.warn_c) return "warn";
  return "ok";
}

function moduleNode(m) {
  if (!m) return el("span", {class: "muted"}, "nothing known about the module");
  if (typeof m.temp_c !== "number") {       /* e.g. a DAC cable: it has a name but no sensor */
    const nm = [m.vendor, m.part].filter(Boolean).join(" ");
    return el("span", {class: "muted"}, (nm ? nm + " – " : "") + "no temperature sensor" + (nm ? "" : " (cable without one)"));
  }
  const lo = 20, hi = (m.alarm_c != null ? m.alarm_c : m.warn_c != null ? m.warn_c + 8 : m.temp_c + 10) + 4;
  const pos = (t) => Math.max(0, Math.min(100, ((t - lo) / (hi - lo)) * 100));
  const cls = tempClass(m);
  const lim = [m.warn_c != null ? `warning at ${m.warn_c} °C` : "", m.alarm_c != null ? `link goes down at ${m.alarm_c} °C` : ""]
    .filter(Boolean).join(" · ");
  const bar = el("div", {class: "tbar", title: m.label || "Module"}, el("div", {class: `fill ${cls}`, style: `width:${pos(m.temp_c)}%`}),
    ...[m.warn_c, m.alarm_c].filter((x) => x != null).map((x) => el("i", {class: "mark", style: `left:${pos(x)}%`})));
  const info = [[m.vendor, m.part].filter(Boolean).join(" "), m.highest_c != null ? `highest seen ${m.highest_c} °C` : ""].filter(Boolean).join(" · ");
  return el("div", {}, el("span", {class: `tval ${cls}`}, `${m.temp_c.toFixed(1)} °C`), " ", el("span", {class: "muted"}, lim), bar,
    info ? el("small", {}, info) : null);
}

/* the network card's chip: temperature against the limit the firmware warns at (ConnectX-5 and newer have a sensor;
 * a ConnectX-4 only reports heat in the kernel log) */
function chipNode(c) {
  if (!c) return el("span", {class: "muted"}, "no chip sensor (the driver offers none)");
  const ev = c.event && c.event_text ? el("small", {}, c.event_text) : null;
  if (typeof c.temp_c !== "number") return el("div", {}, el("span", {class: "tval alarm"}, "too hot"), ev);
  const lim = c.limit_c != null ? c.limit_c : null;
  const cls = c.event || (lim != null && c.temp_c >= lim) ? "alarm" : lim != null && c.temp_c >= lim - 10 ? "warn" : "ok";
  const hi = lim != null ? lim + 5 : c.temp_c + 10, lo = 20;
  const pos = (t) => Math.max(0, Math.min(100, ((t - lo) / (hi - lo)) * 100));
  const bar = el("div", {class: "tbar", title: c.label || "Chip"}, el("div", {class: `fill ${cls}`, style: `width:${pos(c.temp_c)}%`}),
    ...(lim != null ? [el("i", {class: "mark", style: `left:${pos(lim - 10)}%`}), el("i", {class: "mark", style: `left:${pos(lim)}%`})] : []));
  return el("div", {}, el("span", {class: `tval ${cls}`}, `${c.temp_c.toFixed(1)} °C`), " ",
    el("span", {class: "muted"}, lim != null ? `limit ${lim} °C` + (c.highest_c != null ? ` · highest seen ${c.highest_c} °C` : "") : ""),
    bar, ev);
}

/* the card itself is gone: what was seen and what to do */
function cardNode(n) {
  return el("div", {}, el("span", {class: "tval alarm"}, n.power_cycle === false ? "Not available" : "Failed"),
    " · " + (n.likely_overheated ? "likely overheated · " : "") + (n.why || ""),
    el("small", {}, n.power_cycle === false ? "Reload the driver or power-cycle the computer." : "Power-cycle the computer: a reconnect cannot bring the card back."),
    n.detail ? el("small", {}, n.detail) : null);
}

function lastDownNode(L) {
  const d = L.last_down;
  if (!d) return el("span", L.downs ? {} : {class: "muted"},
    L.downs ? `${L.downs} link down${L.downs > 1 ? "s" : ""} since the program started` : "none since the program started");
  const dur = d.back_at ? `down for ${fmtSecs(d.down_s != null ? d.down_s : d.back_at - d.at)}` : "still down";
  const t = typeof d.temp_c === "number" ? ` (${d.temp_c.toFixed(1)} °C)` : "";
  return el("div", {}, `${fmtClock(d.at)} – ${d.reason || "link down"}${t}, ${dur}`,
    L.downs > 1 ? el("small", {}, `${L.downs} link downs since the program started`) : null,
    d.detail ? el("small", {}, d.detail) : null);
}

/* one side of the cable: heading, level pill, state / module / last down rows */
function linkSide(title, L) {
  const lvl = L.known === false ? "unknown" : L.level || "unknown";
  const head = el("h3", {}, title, el("span", {class: `lvl ${lvl}`}, LEVEL_WORD[lvl] || lvl));
  if (L.known === false) return el("div", {class: "linkside"}, head, el("p", {class: "muted small"}, L.message || ""));
  const where = [L.rdma_dev && L.rdma_dev + (L.port != null ? ` port ${L.port}` : ""), L.netdev, L.link_ip].filter(Boolean).join(" · ");
  const rows = [
    ["Link", el("div", {}, el("span", {class: "tval" + (L.up ? "" : " alarm")}, L.up ? "Up" : "Down"),
      " · " + [L.state, L.rate].filter(Boolean).join(" · "), where ? el("small", {}, where) : null)],
    ...(L.nic && L.nic.failed ? [["Network card", cardNode(L.nic)]] : []),
    ["Module", moduleNode(L.module)],
    ["Card chip", chipNode(L.chip)],
    ["Last link down", lastDownNode(L)],
  ];
  return el("div", {class: "linkside"}, head, el("dl", {class: "kv"},
    ...rows.flatMap(([k, v]) => [el("dt", {}, k), el("dd", {}, v)])));
}

let shownLink = "";
function renderLink(st) {
  const own = st.link && typeof st.link === "object" ? st.link : null;
  const peer = st.peer_link && typeof st.peer_link === "object" ? st.peer_link : null;
  const warn = typeof st.link_warning === "string" ? st.link_warning : "";
  const banner = $("#link-banner");
  banner.hidden = !warn;
  banner.textContent = warn;
  banner.className = "banner" + ([own, peer].some((l) => l && (l.level === "alarm" || l.level === "down" || l.level === "failed")) ? " bad" : "");
  /* the card shows when a daemon reports a link (an old daemon: nothing); a known:false side shows only with a message */
  const sides = [["This computer", own], ["The other computer", peer]].filter(([, l]) => l && (l.known !== false || l.message));
  $("#link-card").hidden = sides.length === 0;
  const sig = JSON.stringify(sides);
  if (sig === shownLink) return;
  shownLink = sig;
  const worst = ["failed", "down", "alarm", "warn", "ok"].find((lv) => sides.some(([, l]) => l.level === lv)) || "unknown";
  $("#link-lvl").className = `lvl ${worst}`;
  $("#link-lvl").textContent = LEVEL_WORD[worst];
  $("#link-body").replaceChildren(...sides.map(([t, l]) => linkSide(t, l)));
}

/* a host's display name: its configured name ("Seat 1"), else the seat's own ("RDMA Display - Seat 1"),
 * else its host name. The card of a seat is no part of the name: it can start on any card (cardBar) */
function hostTitle(h) {
  const i = h.info || {};
  return h.name || i.seat_name || i.host_name || h.address;
}

/* ---------------- card choice of a multiseat host (/api/seats, clientd -> rdx-seatmgr) ---------------- */

let seats = null;            // GET /api/seats: {enabled, cards, seats, hosts: [{index, seat}], error}
const seatWork = {};         // host index -> what its card switch is doing now ("Starting …")
const cardPick = {};         // host index -> the card picked in its list (kept over re-renders)
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function pollSeats() {
  try {
    const r = await fetch("/api/seats");
    seats = r.ok ? await r.json() : null;
  } catch (e) { seats = null; }
  if (state) { shownOthers = ""; shownActiveBar = ""; renderOtherHosts(state); }
}

const CARD_STATE = {free: "free", llm: "used by an LLM server", busy: "in use by other programs", booked: "booked in the GPU booking service",
                    unusable: "not usable"};

function seatOf(h) {
  if (!seats || !seats.enabled) return null;
  const e = (seats.hosts || []).find((x) => x.index === h.index);
  return e && e.seat ? (seats.seats || []).find((s) => s.seat === e.seat) || null : null;
}

function cardWord(c, seat) {
  if (c.status === "seat") return c.seat === seat.seat ? "this seat" : `Seat ${c.seat}`;
  return CARD_STATE[c.status] || c.status;
}

/* the line under a seat's name: its card now, a list of the cards it may take with their state, Start */
function cardBar(h, busy) {
  const seat = seatOf(h);
  if (!seat) return null;
  const cards = (seats.cards || []).filter((c) => (seat.gpus || []).includes(c.pci));
  const cur = cards.find((c) => c.pci === seat.gpu);
  const work = seatWork[h.index];
  const usable = (c) => c.status === "free" || (c.status === "seat" && c.seat === seat.seat);
  const pick = cardPick[h.index] && cards.some((c) => c.pci === cardPick[h.index]) ? cardPick[h.index]
    : cur && usable(cur) ? cur.pci : (cards.find(usable) || cur || {}).pci;
  const sel = el("select", {class: "cardsel", disabled: !!work || busy, title: "The graphics card this seat starts on",
    onchange: (ev) => { cardPick[h.index] = ev.target.value; }},
    ...cards.map((c) => el("option", {value: c.pci, selected: c.pci === pick, title: c.why || ""},
      `${c.short} (${c.pci.replace(/^0000:/, "")}) – ${cardWord(c, seat)}`)));
  const now = seat.running && cur ? `on ${cur.short}` : seat.running ? "running" : "stopped";
  const btn = el("button", {class: "ghost", type: "button", disabled: !!work || busy,
    title: busy ? "Disconnect from the current stream first" : "",
    onclick: () => startOnCard(h, sel.value)}, seat.running ? "Restart on this card" : "Start");
  const why = cards.find((c) => c.pci === pick && !usable(c));
  return el("div", {class: "cardbar"}, el("span", {class: "cardnow"}, now), sel, btn,
    work ? el("small", {class: "cardwork"}, work) : why && why.why ? el("small", {class: "muted"}, why.why) : null);
}

async function startOnCard(h, pci, force = false) {
  const seat = seatOf(h);
  const card = ((seats && seats.cards) || []).find((c) => c.pci === pci);
  const label = card ? card.short : pci;
  const name = hostTitle(h);
  if (seat && seat.running && !force && !confirm(`Restart ${name} on the ${label}? Apps running there are closed.`)) return;
  seatWork[h.index] = `Starting ${name} on the ${label} …`;
  shownOthers = shownActiveBar = "";
  if (state) renderOtherHosts(state);
  try {
    const r = await fetch("/api/seats/start", {method: "POST", headers: {"Content-Type": "application/json"},
                                               body: JSON.stringify({host: h.index, gpu: pci, force})});
    const j = await r.json().catch(() => ({error: "bad reply"}));
    if (!r.ok) {
      delete seatWork[h.index];
      if (r.status === 409 && /is in use/.test(j.error || "") && !force &&
          confirm(`${j.error}\n\nStop it anyway? Its apps are closed.`)) return startOnCard(h, pci, true);
      alert(j.error || r.statusText);
      return;
    }
    seatWork[h.index] = `Waiting for ${name} …`;
    shownOthers = shownActiveBar = "";
    let up = null;
    for (let t = 0; t < 120 && !up; t++) {
      await sleep(1000);
      const st = await api("/api/state").catch(() => null);
      const hh = st && (st.hosts || []).find((x) => x.index === h.index);
      if (hh && hh.connected && hh.info && (!hh.info.gpu || hh.info.gpu === pci)) up = hh;
    }
    if (!up) { alert(`${name} started on the ${label}, but its RDMA Display program did not answer within 2 minutes.`); return; }
    const apps = up.info.apps || [];
    const app = state && apps.some((a) => a.id === state.config.app) ? state.config.app : apps.length ? apps[0].id : null;
    if (app) await start(app, null, h.index);
  } catch (e) {
    alert(e.message);
  } finally {
    delete seatWork[h.index];
    pollSeats();
  }
}

/* the other configured hosts (seats): one group of tiles each; starting one switches to it */
let shownOthers = "";
let shownActiveBar = "";
function renderOtherHosts(st) {
  const hosts = st.hosts || [];
  const others = hosts.filter((h) => !h.active);
  const busy = st.session.state !== "idle";
  const head = $("#apps-host");
  const act = hosts.find((h) => h.active);
  head.hidden = !(others.length || seatOf(act || {})) || !act;
  const gpuName = (h) => (h.info && h.info.gpu_name ? h.info.gpu_name.replace(/^NVIDIA (GeForce )?/, "") : "");
  const actSeat = seatOf(act || {});
  if (!head.hidden) head.replaceChildren(document.createTextNode(hostTitle(act)),
    el("small", {}, act.connected ? ["connected", gpuName(act)].filter(Boolean).join(" · ")
                                  : actSeat && !actSeat.running ? "stopped" : "offline"));
  const seatSig = JSON.stringify([seats, seatWork]);
  const barSig = JSON.stringify([act && act.index, busy, seatSig]);
  if (barSig !== shownActiveBar) {
    shownActiveBar = barSig;
    const bar = act ? cardBar(act, busy) : null;
    $("#apps-cardbar").replaceChildren(...(bar ? [bar] : []));
  }
  const sig = JSON.stringify([others.map((h) => [h.index, h.connected, h.info && h.info.apps,
                                                   suspendedOf(h.info).map((x) => [x.app, x.mode]),
                                                   hostTitle(h), h.error, gpuName(h)]), busy, seatSig]);
  if (sig === shownOthers) return;
  shownOthers = sig;
  $("#other-hosts").replaceChildren(...others.map((h) => {
    const info = h.info || {};
    const apps = info.apps || [];
    const outs = Object.fromEntries((info.outputs || []).map((o) => [o.name, o]));
    const susById = Object.fromEntries(suspendedOf(info).map((x) => [x.app, x]));
    const tiles = apps.map((a) => {
      const v = appView(a, outs);
      const sus = susById[a.id];
      const isSus = !!sus;
      const tile = el("button", {
        class: "tile" + (isSus ? " suspended" : ""), type: "button", disabled: busy,
        title: busy ? "Disconnect from the current stream first" : `${isSus ? "Resume" : "Start"} ${v.title} on ${hostTitle(h)}`,
        onclick: () => start(a.id, null, h.index),
      }, tileIcon(a, v, h.index), el("b", {}, v.title),
         isSus ? el("span", {class: "run-badge"}, "Running · Resume") : null,
         el("small", {}, isSus && sus.mode ? fmtMode(sus.mode.w, sus.mode.h, sus.mode.hz) : v.sub));
      if (!isSus) return el("div", {class: "tilewrap"}, tile);
      const quit = el("button", {class: "quit", type: "button", disabled: busy, title: `Quit ${v.title} on ${hostTitle(h)}`,
        onclick: (ev) => { ev.stopPropagation(); quitApp(a.id, v.title, h.index); }}, "Quit");
      return el("div", {class: "tilewrap"}, tile, quit);
    });
    const sb = seatOf(h);
    const note = !h.connected ? (sb && !sb.running ? "Stopped: pick a card and start it." : `Not reachable (${h.address}:${h.port})`)
      : apps.length ? "" : "Offers nothing to stream yet.";
    return el("section", {},
      el("h2", {class: "hosthead"}, hostTitle(h), el("small", {}, h.connected ? gpuName(h) : sb && !sb.running ? "stopped" : "offline")),
      cardBar(h, busy),
      el("div", {class: "tiles"}, ...tiles),
      note ? el("p", {class: "muted center"}, note) : null);
  }));
}

function appTitle(id, byId, outs) {
  const a = byId[id];
  return a ? appView(a, outs).title : id;
}

function fmtDuration(s) {           /* whole minutes: a once-per-second counter makes the page jitter */
  if (s < 60) return "less than a minute";
  if (s < 3600) return `${Math.round(s / 60)} min`;
  return `${Math.floor(s / 3600)} h ${Math.round((s % 3600) / 60)} min`;
}

/* ---------------- settings ---------------- */

function modeOptions(modes, cur, all) {
  // per resolution: the highest native refresh and the highest boosted (derived) one; all modes on request
  const groups = new Map();
  for (const m of modes) {
    const k = `${m.w}x${m.h}`;
    if (!groups.has(k)) groups.set(k, {w: m.w, h: m.h, list: []});
    groups.get(k).list.push(m);
  }
  const best = [...modes].sort((a, b) => b.hz - a.hz || b.w * b.h - a.w * a.h)[0];
  const key = (m) => `${m.w}x${m.h}@${m.hz}`;
  /* a window client lists its monitors' own modes ("monitor"): "match this monitor" */
  const win = modes.some((m) => m.monitor);
  const label = (m) => fmtMode(m.w, m.h, m.hz) + (m.derived ? " · boosted" : "") +
    (m.monitor ? ` · match monitor ${m.monitor}${m.preferred ? " (the window's)" : ""}` :
      (best && key(m) === key(best) ? " (recommended)" : "") + (m.preferred ? " (screen default)" : ""));
  const opts = [el("option", {value: ""}, win ? "Automatic: the window's monitor, native and full screen"
                                              : "Automatic (screen default)")];
  const seen = new Set();
  const sorted = [...groups.values()].sort((a, b) => b.w * b.h - a.w * a.h);
  for (const g of sorted) {
    let pick = g.list;
    const isCur = cur && cur.w === g.w && cur.h === g.h;
    if (!all && (g.w < 1280 || g.h < 720) && !isCur) continue;   /* small sizes only under "Show all modes" */
    if (!all) {
      const nat = g.list.filter((m) => !m.derived).sort((a, b) => b.hz - a.hz)[0];
      const der = g.list.filter((m) => m.derived).sort((a, b) => b.hz - a.hz)[0];
      pick = [der, nat].filter(Boolean).filter((m, i, arr) => !(i && m.hz === arr[0].hz && m.derived === arr[0].derived));
      if (cur && cur.w === g.w && cur.h === g.h) {
        const c = g.list.find((m) => m.hz === cur.hz);
        if (c && !pick.includes(c)) pick.push(c);
      }
    } else {
      pick = [...g.list].sort((a, b) => b.hz - a.hz);
    }
    const og = el("optgroup", {label: `${g.w} × ${g.h}`});
    for (const m of pick) { og.append(el("option", {value: key(m)}, label(m))); seen.add(key(m)); }
    opts.push(og);
  }
  if (cur && cur.hz && !seen.has(key(cur))) opts.push(el("option", {value: key(cur)}, fmtMode(cur.w, cur.h, cur.hz) + " (saved)"));
  return {opts, best};
}

function inputLabel(d) {
  const n = d.name || d.path;
  if (/consumer control/i.test(n)) return {title: n.replace(/\s*consumer control/i, ""), what: "Media keys"};
  if (/system control/i.test(n)) return {title: n.replace(/\s*system control/i, ""), what: "Power keys"};
  if (d.kind === "mouse" || /mouse|touchpad|trackpad/i.test(n)) return {title: n.replace(/\s*mouse$/i, ""), what: "Mouse"};
  if (d.kind === "kbd" || /keyboard/i.test(n)) return {title: n, what: "Keyboard"};
  return {title: n, what: "Input device"};
}

function usbLabel(d) {
  const words = (d.name || d.id).split(/\s+/);
  const title = words.filter((w, i) => i === 0 || w.toLowerCase() !== words[i - 1].toLowerCase()).join(" ");
  const n = `${d.name} ${d.vid}`;
  let what = "USB device";
  if (/controller|gamepad|joystick|xbox|dualsense|dualshock|8bitdo|wheel/i.test(n) || /^(045e|054c|057e|2dc8|28de)$/.test(d.vid || "")) what = "Gamepad";
  else if (/headset|headphone|barracuda|audio|microphone|speaker/i.test(n)) what = "Headset / audio";
  else if (/receiver|dongle|unifying/i.test(n)) what = "Wireless receiver";
  else if (/keyboard/i.test(n)) what = "Keyboard";
  else if (/mouse|deathadder|g\d{3}/i.test(n)) what = "Mouse";
  else if (/webcam|camera/i.test(n)) what = "Camera";
  else if (/storage|disk|flash|card reader/i.test(n)) what = "Storage";
  return {title, what};
}

function switchRow(title, sub, name, value, checked, disabled) {
  return el("div", {class: "dev" + (disabled ? " dim" : "")},
    el("span", {class: "what"}, el("span", {}, title), sub ? el("small", {}, sub) : null),
    el("label", {class: "switch" + (disabled ? " dim" : ""), title: disabled ? sub : ""},
      el("input", {type: "checkbox", name, value, checked, disabled}), el("span", {})));
}

function fillSettings(st) {
  const c = st.config;
  const modes = (st.lists.modes && st.lists.modes.modes) || [];
  const {opts} = modeOptions(modes, c.mode, $("#all-modes").checked);
  form.mode.replaceChildren(...opts);
  form.mode.value = c.mode && c.mode.hz ? `${c.mode.w}x${c.mode.h}@${c.mode.hz}` : "";
  form.querySelector(`input[name=pacing][value="${c.pacing === "smooth" ? "smooth" : "latency"}"]`).checked = true;
  form.cursor.checked = c.cursor;
  /* a window client's pointer (config "pointer"): only for "sink": "window" and a daemon that knows it */
  $("#pointer-choice").hidden = c.sink !== "window" || !("pointer" in c);
  const wfs = $("#window-fullscreen");
  wfs.hidden = c.sink !== "window" || !("window_fullscreen" in c);
  form.window_fullscreen.checked = c.window_fullscreen !== false;
  form.vrr.checked = c.vrr !== false;
  /* a window client: the game monitor by lease, the desktop hotkey */
  $("#game-monitor").hidden = c.sink !== "window" || !("game_monitor" in c);
  $("#desktop-hotkey").hidden = c.sink !== "window" || !("desktop_hotkey" in c);
  const gm = ["auto", "lease", "window"].includes(c.game_monitor) ? c.game_monitor : "auto";
  form.querySelector(`input[name=game_monitor][value="${gm}"]`).checked = true;
  if (document.activeElement !== form.desktop_hotkey) form.desktop_hotkey.value = c.desktop_hotkey || "ctrl+alt+h";
  form.vrr.closest("label").hidden = !("vrr" in c);
  const pv = ["auto", "absolute", "relative"].includes(c.pointer) ? c.pointer : "auto";
  form.querySelector(`input[name=pointer][value="${pv}"]`).checked = true;
  form.tearing.checked = !!c.tearing;
  form.match_host.checked = !!c.match_host && !c.tearing;
  form.match_host.disabled = !!c.tearing;
  form.match_host.closest("label").classList.toggle("dim", !!c.tearing);
  form.sound.checked = c.audio && c.audible;
  form.audio.checked = c.audio;
  form.input.checked = c.input;
  form.mic.checked = c.mic !== false;
  const srcs = st.lists.mic_sources || [];
  const micOpts = [el("option", {value: ""}, "Default")].concat(srcs.map((m) => el("option", {value: m.name}, m.description || m.name)));
  if (c.mic_source && !srcs.some((m) => m.name === c.mic_source))
    micOpts.push(el("option", {value: c.mic_source}, `${c.mic_source} (not connected)`));
  form.mic_source.replaceChildren(...micOpts);
  form.mic_source.value = c.mic_source || "";
  form.mic_source.disabled = !form.mic.checked;
  $("#clip-card").hidden = !("clipboard" in c);
  form.clipboard.checked = c.clipboard !== false;
  form.clipboard_primary.checked = c.clipboard_primary !== false;
  form.clipboard_primary.disabled = !form.clipboard.checked;

  const chosen = new Set(c.input_devices);
  $("#inputs").replaceChildren(...st.lists.inputs.map((d) => {
    const l = inputLabel(d);
    return switchRow(l.title, l.what, "input_dev", d.path, chosen.size === 0 || chosen.has(d.path), !c.input);
  }));

  const usbSel = new Map(c.usb.map((u) => { const i = u.lastIndexOf(":"); return i > 0 ? [u.slice(0, i), u.slice(i + 1)] : [u, null]; }));
  const devs = st.lists.usb.devices || [];
  const titles = devs.map((d) => usbLabel(d).title);
  $("#usb").replaceChildren(...(devs.length ? devs.map((d, i) => {
    const l = usbLabel(d);
    if (titles.indexOf(l.title) !== titles.lastIndexOf(l.title)) l.what += ` · USB port ${d.busid || d.id}`;
    const busy = d.in_use_by ? (d.in_use_by === "input" ? "already used as this computer's keyboard/mouse" : `in use by ${d.in_use_by}`) : "";
    return switchRow(l.title, busy || l.what, "usb_dev", d.id, usbSel.has(d.id), !!busy);
  }) : [el("p", {class: "muted small"}, st.lists.usb.note || "No USB devices found.")]));
  // advanced: how each switched-on device is passed
  $("#usb-adv").replaceChildren(...devs.filter((d) => usbSel.has(d.id)).map((d) => {
    const kinds = d.kinds && d.kinds.length ? d.kinds : [d.kind];
    const picked = usbSel.get(d.id) || d.kind;
    return el("label", {class: "field"}, `${usbLabel(d).title}: pass as `,
      el("select", {name: "usb_kind", "data-id": d.id, class: "inline"},
        ...kinds.map((k) => el("option", {value: k, selected: k === picked},
          k === "hid" ? "input device (HID)" : "whole USB device (usbip)"))));
  }));

  for (const k of ["host", "host_web_port", "fps", "gpu_prio", "rdma_dev", "link_ip", "mtu", "card", "sink_vt", "desk_vt"]) {
    if (form[k] && c[k] !== undefined) form[k].value = String(c[k]);
  }
  if (form.fps.value !== String(c.fps)) { form.fps.append(el("option", {value: String(c.fps)}, String(c.fps))); form.fps.value = String(c.fps); }
  renderProcs(st.session);
}

function readSettings() {
  const mv = form.mode.value;
  const mm = mv && mv.match(/^(\d+)x(\d+)@(\d+)$/);
  const devsOn = [...form.querySelectorAll("input[name=input_dev]")];
  const on = devsOn.filter((i) => i.checked).map((i) => i.value);
  const num = (k) => { const v = parseInt(form[k].value, 10); return Number.isFinite(v) ? v : state.config[k]; };
  return {
    mode: mm ? {w: +mm[1], h: +mm[2], hz: +mm[3]} : null,
    pacing: form.querySelector("input[name=pacing]:checked").value,
    cursor: form.cursor.checked,
    ...("vrr" in state.config ? {vrr: form.vrr.checked} : {}),
    ...("game_monitor" in state.config ? {game_monitor: (form.querySelector("input[name=game_monitor]:checked") || {}).value || "auto"} : {}),
    ...("desktop_hotkey" in state.config ? {desktop_hotkey: form.desktop_hotkey.value.trim() || "ctrl+alt+h"} : {}),
    ...("window_fullscreen" in state.config ? {window_fullscreen: form.window_fullscreen.checked} : {}),
    ...("pointer" in state.config ? {pointer: (form.querySelector("input[name=pointer]:checked") || {}).value || "auto"} : {}),
    tearing: form.tearing.checked,
    match_host: form.match_host.checked && !form.tearing.checked,
    audio: form.sound.checked || form.audio.checked,
    audible: form.sound.checked,
    input: form.input.checked,
    mic: form.mic.checked,
    mic_source: form.mic_source.value,
    ...("clipboard" in state.config ? {clipboard: form.clipboard.checked, clipboard_primary: form.clipboard_primary.checked} : {}),
    input_devices: on.length === devsOn.length ? [] : on,
    usb: [...form.querySelectorAll("input[name=usb_dev]:checked")].map((i) => {
      const k = form.querySelector(`select[name=usb_kind][data-id="${CSS.escape(i.value)}"]`);
      return k ? `${i.value}:${k.value}` : i.value;
    }),
    host: form.host.value.trim() || state.config.host,
    host_web_port: num("host_web_port"),
    fps: +form.fps.value,
    gpu_prio: form.gpu_prio.value,
    rdma_dev: form.rdma_dev.value.trim() || state.config.rdma_dev,
    link_ip: form.link_ip.value.trim() || state.config.link_ip,
    mtu: num("mtu"),
    card: form.card.value.trim() || state.config.card,
    sink_vt: num("sink_vt"),
    desk_vt: num("desk_vt"),
  };
}

async function save() {
  clearTimeout(saveTimer);
  saveTimer = null;
  const cfg = await api("/api/config", readSettings());
  state.config = cfg;
  const t = $("#saved");
  t.hidden = false;
  clearTimeout(t._h);
  t._h = setTimeout(() => { t.hidden = true; }, 1200);
}

function saveSoon(delay = 250) {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => save().catch((e) => alert(e.message)).then(poll), delay);
}

form.addEventListener("change", (ev) => {
  const t = ev.target;
  if (t.id === "all-modes") { shownSettings = ""; fillSettings(state); return; }
  if (t.name === "sound") { form.audio.checked = t.checked; }
  if (t.name === "tearing") {               /* tearing and matching the host's refresh exclude each other */
    if (t.checked) form.match_host.checked = false;
    form.match_host.disabled = t.checked;
    form.match_host.closest("label").classList.toggle("dim", t.checked);
  }
  saveSoon(t.tagName === "INPUT" && t.type !== "checkbox" && t.type !== "radio" ? 600 : 50);
});
form.addEventListener("submit", (ev) => { ev.preventDefault(); saveSoon(0); });
// "sound" is one switch for receiving + playing; the advanced checkbox receives without playing
form.addEventListener("change", (ev) => {
  if (ev.target.name === "audio" && !ev.target.checked) form.sound.checked = false;
}, true);

/* the clipboard card's line: what the running session does with it */
function renderClip(st) {
  const s = st.session || {}, c = s.clipboard;
  let t = "";
  if (s.state === "running" && c) {
    if (!c.available) t = c.on ? "This session has no clipboard (it was off at the start, or the other computer does not offer it): it applies from the next start." : "Off in this session.";
    else if (!c.on) t = "Off in this session.";
    else t = c.running ? "Shared with the app now." : "Not running in this session (see the log of the part \u201cclip\u201d).";
  }
  $("#clip-state").textContent = t;
}

/* ---------------- parts and logs (advanced) ---------------- */

function renderProcs(s) {
  const tb = $("#procs tbody");
  tb.replaceChildren(...s.procs.map((p) => el("tr", {},
    el("td", {}, p.name + (p.running ? "" : " (ended)")),
    el("td", {}, String(p.pid)),
    el("td", {}, (p.cpu_pct || 0).toFixed(1) + " %"),
    el("td", {}, el("button", {class: "ghost", type: "button", onclick: () => showLog(p.name)}, "log")))));
  $("#procs").hidden = s.procs.length === 0;
}

async function showLog(proc) {
  logProc = logProc === proc ? null : proc;
  await refreshLog();
}

async function refreshLog() {
  const pre = $("#log");
  if (!logProc) { pre.hidden = true; return; }
  try {
    const r = await fetch(`/api/logs?proc=${encodeURIComponent(logProc)}`);
    pre.textContent = `── ${logProc} ──\n` + (r.ok ? await r.text() : "(no log)");
  } catch (e) { pre.textContent = String(e); }
  pre.hidden = false;
  pre.scrollTop = pre.scrollHeight;
}

/* ---------------- actions, polling, navigation ---------------- */

/* the apps running in a host's background: its list ("suspended_apps"), an older host's one "suspended" */
function suspendedOf(info, h) {
  const list = (h && h.suspended_apps) || (info && info.suspended_apps);
  if (Array.isArray(list) && (list.length || !(h ? h.suspended : info && info.suspended))) return list;
  const one = h ? h.suspended : info && info.suspended;
  return one ? [one] : [];
}

async function start(app, suspendedTitle, host) {
  if (suspendedTitle && !confirm(`${suspendedTitle} is still running on the other computer and will be closed. Start anyway?`)) return;
  try {
    if (saveTimer) await save();
    await api("/api/session/start", host === undefined ? {app} : {app, host});
  } catch (e) { alert(e.message); }
  poll();
}

async function quitApp(app, title, host) {
  if (!confirm(`Quit ${title}? Unsaved progress may be lost.`)) return;
  try { await api("/api/apps/quit", host === undefined ? {app} : {app, host}); } catch (e) { alert(e.message); }
  setTimeout(poll, 300);
}

$("#stop").addEventListener("click", async () => { await api("/api/session/stop", {}).catch(() => {}); poll(); });
$("#restart-host").addEventListener("click", async () => {
  if (!confirm("Restart the RDMA Display program on the other computer? A running stream ends and a paused app there is closed.")) return;
  try { await api("/api/host/restart", {}); } catch (e) { alert(e.message); }
  setTimeout(poll, 3000);
});
$("#refresh").addEventListener("click", async () => { await api("/api/refresh", {}).catch(() => {}); setTimeout(poll, 400); });

function show(view) {
  const settings = view === "settings";
  $("#view-home").hidden = settings;
  $("#view-settings").hidden = !settings;
  $("#nav-home").classList.toggle("sel", !settings);
  $("#nav-settings").classList.toggle("sel", settings);
  if (settings && state) { shownSettings = ""; poll(); }
}
$("#nav-home").addEventListener("click", () => { location.hash = ""; });
$("#nav-settings").addEventListener("click", () => { location.hash = "settings"; });
window.addEventListener("hashchange", () => show(location.hash === "#settings" ? "settings" : "home"));

async function poll() {
  try {
    state = await api("/api/state");
  } catch (e) {
    const pill = $("#host-pill");
    pill.textContent = "This computer's RDMA Display service is not running";
    pill.className = "pill bad";
    return;
  }
  renderHome(state);
  renderOtherHosts(state);
  try { renderLink(state); } catch (e) { $("#link-card").hidden = true; $("#link-banner").hidden = true; }
  // refill the settings only when something changed and the user is not in the middle of an edit
  const sig = JSON.stringify([state.config, state.lists, $("#all-modes").checked]);
  const editing = saveTimer || (document.activeElement && form.contains(document.activeElement) &&
                                ["INPUT", "SELECT"].includes(document.activeElement.tagName) &&
                                !["checkbox", "radio"].includes(document.activeElement.type));
  if (!$("#view-settings").hidden && sig !== shownSettings && !editing) {
    shownSettings = sig;
    fillSettings(state);
  } else if (!$("#view-settings").hidden) {
    renderProcs(state.session);
  }
  if (!$("#view-settings").hidden) renderClip(state);
  if (logProc) refreshLog();
}

show(location.hash === "#settings" ? "settings" : "home");
poll();
setInterval(poll, 1000);
pollSeats();
setInterval(() => { if (!document.hidden) pollSeats(); }, 5000);
