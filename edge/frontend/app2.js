// =========================================================
// EDGE DASHBOARD — STABLE + DEMO SAFE
// Fixes:
// - Ensures popup globals always export (even if later code fails)
// - Removes YouTube double-load (loads iframe API once here)
// - Stops camera reader.js /whep spam by NOT embedding that camera webpage
//   (still allows opening camera in new tab)
// - Adds basic boot markers for debugging
// =========================================================

console.log("✅ app.js start", new Date().toISOString());
window.__APPJS_OK__ = true;

window.addEventListener("error", (e) => {
  console.error("🔥 window error:", e.message, e.filename, e.lineno, e.colno);
});
window.addEventListener("unhandledrejection", (e) => {
  console.error("🔥 unhandled rejection:", e.reason);
});

const CONFIG = {
  MQTT_HOST: window.location.hostname || "localhost",
  MQTT_PORT: 9001,
  TEAM_TOPIC: "edge/status/Team_A/#",
  ALERT_TOPIC: "edge/alerts/Team_A/#",
  INCIDENT_TOPIC: "edge/incident/Team_A",

  // ===== ACTION POPUP (ORION) =====
  ORION_BASE: "http://192.168.2.12:1026",
  ACTION_TYPE: "CommandAction",
  ACTION_TEAM: "Team_A",
  ACTION_POLL_MS: 1200,
};

const FF_ORDER = ["FF_A", "FF_B", "FF_C", "FF_D"];

// ===== TRAILS CONFIG =====
const TRAIL_POINTS = 10;
const TRAIL_MIN_METERS = 3;
const TRAIL_OPTS = { weight: 4, opacity: 0.75 };
const trails = {}; // ffId -> { points: [[lat,lon],...], line: L.Polyline }

// ===== Render throttle =====
let renderTimer = null;
function scheduleRender() {
  if (renderTimer) return;
  renderTimer = setTimeout(() => {
    renderTimer = null;
    renderAllCards();
  }, 200);
}

// =========================================================
// ACTION POPUP — robust overlay
// =========================================================
const actionPopupState = { lastId: null };

function popupEls() {
  return {
    root: document.getElementById("actionPopup"),
    card: document.getElementById("actionPopupCard"),
    title: document.getElementById("actionPopupTitle"),
    msg: document.getElementById("actionPopupMsg"),
    ok: document.getElementById("actionPopupAcknowledge"),
    x: document.getElementById("actionPopupX"),
  };
}

function forcePopupOverlayStyleOnce() {
  const { root } = popupEls();
  if (!root || root.dataset.overlayForced) return;
  root.dataset.overlayForced = "1";

  root.style.position = "fixed";
  root.style.inset = "0";
  root.style.zIndex = "2147483647";
  root.style.background = "rgba(0,0,0,.55)";
  root.style.alignItems = "center";
  root.style.justifyContent = "center";
  root.style.display = "none";
}

function closePopup() {
  const { root } = popupEls();
  if (!root) return;
  root.classList.add("hidden");
  root.style.display = "none";
}

function showPopup(title, message) {
  const { root, title: t, msg } = popupEls();
  if (!root || !t || !msg) return;

  forcePopupOverlayStyleOnce();

  t.textContent = title || "ALERT";
  msg.textContent = message || "";

  root.classList.remove("hidden");
  root.style.display = "flex";
}

function wirePopupOnce() {
  const { root, ok, x } = popupEls();
  if (!root || root.dataset.wired) return;
  root.dataset.wired = "1";

  forcePopupOverlayStyleOnce();

  ok?.addEventListener("click", closePopup);
  x?.addEventListener("click", closePopup);

  root.addEventListener("click", (e) => {
    if (e.target === root) closePopup();
  });

  window.addEventListener("keydown", (e) => {
    if (e.key === "Escape") closePopup();
  });
}

function prettyAction(a) {
  const up = String(a || "").toUpperCase();
  if (up.includes("EVAC")) return "🚨 EVACUATE ORDER";
  if (up.includes("MEDICAL")) return "🩺 MEDICAL ATTENTION";
  if (up.includes("HOLD")) return "🛑 HOLD POSITION";
  if (up.includes("RETREAT")) return "↩️ RETREAT";
  return `📢 ${a || "ACTION"}`;
}

function actionIdTs(id) {
  const m = String(id || "").match(/:(\d+)$/);
  return m ? Number(m[1]) : 0;
}

async function fetchLatestCommandAction() {
  const base = CONFIG.ORION_BASE.replace(/\/+$/, "");
  const url =
    `${base}/v2/entities?type=${encodeURIComponent(CONFIG.ACTION_TYPE)}` +
    `&limit=50&options=keyValues`;

  const r = await fetch(url, { cache: "no-store" });
  if (!r.ok) throw new Error(`Orion HTTP ${r.status}`);

  const arr = await r.json();
  if (!Array.isArray(arr) || !arr.length) return null;

  const teamPrefix = `CommandAction:${CONFIG.ACTION_TEAM}:`;
  const teamActions = arr.filter((e) => String(e?.id || "").startsWith(teamPrefix));
  if (!teamActions.length) return null;

  teamActions.sort((a, b) => actionIdTs(b.id) - actionIdTs(a.id));
  return teamActions[0];
}

async function pollCommandActions() {
  try {
    wirePopupOnce();

    const ent = await fetchLatestCommandAction();
    if (!ent || !ent.id) return;

    if (ent.id === actionPopupState.lastId) return;
    actionPopupState.lastId = ent.id;

    const action = ent.action || "ACTION";
    const ffId = ent.ffId || "ALL";
    const note = ent.note || "";

    const lines = [
      `Target: ${ffId}`,
      note ? `Note: ${note}` : "",
      `Id: ${ent.id}`,
    ].filter(Boolean).join("\n");

    showPopup(prettyAction(action), lines);
  } catch (e) {
    // Keep demo stable; log once if you want:
    // console.warn("CommandAction poll failed:", e?.message || e);
  }
}

async function startCommandActionPolling() {
  try {
    // Prime: read latest action once, but DO NOT show it
    const ent = await fetchLatestCommandAction();
    actionPopupState.lastId = ent?.id || null;
  } catch {
    // ignore
  }

  // Now start polling normally (will only show future actions)
  setInterval(pollCommandActions, CONFIG.ACTION_POLL_MS);
}

// ✅ EXPORT POPUP GLOBALS EARLY (so you can always test from console)
window.showPopup = showPopup;
window.closePopup = closePopup;
window._pollCommandActions = pollCommandActions;
console.log("✅ popup globals exported");

// =========================================================
// CAMERA CONFIG
// =========================================================
const YT_VIDEO_ID = "mphHFk5IXsQ";
const CAMERA_URLS = {
  // IMPORTANT:
  // Your embedded camera page at :8889/cam is running its own reader.js
  // and spamming POST /cam/whep 404 retries.
  // So we DO NOT embed it. We show a "Click to open" tile instead.
  FF_A: { type: "external", url: "http://192.168.2.13:8889/cam" },

  // YouTube segments:
  FF_B: { type: "youtube", videoId: YT_VIDEO_ID, start: 10, dur: 40 },
  FF_C: { type: "youtube", videoId: YT_VIDEO_ID, start: 70, dur: 40 },
  FF_D: { type: "youtube", videoId: YT_VIDEO_ID, start: 130, dur: 40 },
};

function openCamera(ffId) {
  const cam = CAMERA_URLS[ffId];
  if (!cam) return;
  if (cam.type === "youtube") {
    const url = `https://www.youtube.com/watch?v=${cam.videoId}&t=${cam.start || 0}s`;
    window.open(url, "_blank", "noopener");
  } else if (cam.url) {
    window.open(cam.url, "_blank", "noopener");
  }
}

// =========================================================
// STATE
// =========================================================
const state = {
  members: {},
  alertsByFf: {},
  centeredOnce: false,
  incident: null,
};

let map;
const markers = {};
let incidentCircle = null;
let incidentHotRing = null;

// =========================================================
// HELPERS
// =========================================================
function isNum(x) { return typeof x === "number" && Number.isFinite(x); }
function fmt(x, d = 0) { return isNum(x) ? x.toFixed(d) : "—"; }
function sevRank(s) { return ({ ok: 0, warn: 1, danger: 2 }[s] ?? 0); }

// =========================================================
// CLOCK
// =========================================================
function tickClock() {
  const el = document.getElementById("clock");
  if (!el) return;
  el.textContent = new Date().toLocaleString(undefined, {
    hour: "2-digit", minute: "2-digit", second: "2-digit",
    day: "2-digit", month: "short", year: "numeric"
  });
}
setInterval(tickClock, 1000);
tickClock();

// =========================================================
// TRAILS
// =========================================================
function distanceMeters(a, b) {
  const R = 6371000;
  const toRad = x => x * Math.PI / 180;
  const dLat = toRad(b[0] - a[0]);
  const dLon = toRad(b[1] - a[1]);
  const lat1 = toRad(a[0]);
  const lat2 = toRad(b[0]);
  const s = Math.sin(dLat / 2) ** 2 + Math.cos(lat1) * Math.cos(lat2) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(s)));
}

function ensureTrail(ffId) {
  if (!trails[ffId]) {
    trails[ffId] = { points: [], line: L.polyline([], TRAIL_OPTS).addTo(map) };
  }
  return trails[ffId];
}

function pushTrailPoint(ffId, lat, lon) {
  const t = ensureTrail(ffId);
  const p = [lat, lon];
  const last = t.points[t.points.length - 1];
  if (last && distanceMeters(last, p) < TRAIL_MIN_METERS) return;

  t.points.push(p);
  if (t.points.length > TRAIL_POINTS) t.points.shift();
  t.line.setLatLngs(t.points);
}

// =========================================================
// INCIDENT UI
// =========================================================
function updateIncidentUI(pkt) {
  state.incident = pkt;

  const txt = document.getElementById("incidentText");
  if (txt) {
    const lead = isNum(pkt.leadM) ? ` • lead ${Math.round(pkt.leadM)}m` : "";
    txt.textContent = `r=${Math.round(pkt.radiusM)}m${lead}`;
  }

  if (!map) return;
  const center = [pkt.lat, pkt.lon];

  if (!incidentCircle) {
    incidentCircle = L.circle(center, {
      radius: pkt.radiusM, weight: 2, opacity: 0.9, fillOpacity: 0.12
    }).addTo(map);
  } else {
    incidentCircle.setLatLng(center);
    incidentCircle.setRadius(pkt.radiusM);
  }

  const warnRadius = pkt.radiusM + 30;
  if (!incidentHotRing) {
    incidentHotRing = L.circle(center, {
      radius: warnRadius, weight: 2, opacity: 0.5, dashArray: "6 8", fillOpacity: 0.0
    }).addTo(map);
  } else {
    incidentHotRing.setLatLng(center);
    incidentHotRing.setRadius(warnRadius);
  }
}

// =========================================================
// MARKERS
// =========================================================
function markerIcon(sev = "ok") {
  const cls = sev === "danger" ? "ff-danger" : (sev === "warn" ? "ff-warn" : "ff-ok");
  return L.divIcon({
    className: "",
    html: `<div class="ff-marker-wrap ${cls}"></div>`,
    iconSize: [18, 18],
    iconAnchor: [9, 9]
  });
}

function popupHtml(m) {
  const dist = isNum(m.distanceToLeaderM) ? `${Math.round(m.distanceToLeaderM)} m` : "—";
  const inc = isNum(m.distanceToIncidentM) ? `${Math.round(m.distanceToIncidentM)} m` : "—";
  const seen = (m.lastSeenSec ?? null) === null ? "—" : `${m.lastSeenSec}s ago`;
  return `
    <div style="min-width:240px">
      <div style="font-weight:1100; font-size:14px;">${m.name} <span style="color:#54657e;font-weight:900">(${m.ffId})</span></div>
      <div style="margin-top:8px; color:#54657e; font-size:13px; line-height:1.45;">
        ❤️ Pulse: <b>${fmt(m.pulse,0)}</b> <span style="opacity:.85">(${m.pulseLevel||"—"})</span><br/>
        🌡️ Temp: <b>${fmt(m.temp,1)}°C</b> <span style="opacity:.85">(${m.heatLevel||"—"})</span><br/>
        🫁 Smoke: <b>${fmt(m.gas,0)}</b> <span style="opacity:.85">(${m.smokeLevel||"—"})</span><br/>
        📍 Dist (Leader): <b>${dist}</b><br/>
        🔥 Dist (Incident): <b>${inc}</b><br/>
        ⏱ Last: <b>${seen}</b>
      </div>
    </div>
  `;
}

function upsertMarker(m) {
  if (!isNum(m.lat) || !isNum(m.lon)) return;
  const id = m.ffId;

  pushTrailPoint(id, m.lat, m.lon);

  if (!markers[id]) {
    const mk = L.marker([m.lat, m.lon], { icon: markerIcon(m.severity) }).addTo(map);
    mk.bindPopup(popupHtml(m));
    mk.bindTooltip(m.name, { direction: "top", offset: [0, -10], opacity: 0.9 });
    markers[id] = mk;
  } else {
    markers[id].setLatLng([m.lat, m.lon]);
    markers[id].setIcon(markerIcon(m.severity));
    markers[id].getPopup().setContent(popupHtml(m));
    markers[id].setTooltipContent(m.name);
  }
}

// =========================================================
// AUTO-CENTER ON LEADER
// =========================================================
function maybeCenterOnLeader() {
  const a = state.members["FF_A"];
  if (!a || !isNum(a.lat) || !isNum(a.lon)) return;

  if (!state.centeredOnce) {
    map.setView([a.lat, a.lon], 19);
    state.centeredOnce = true;
  }
}

// =========================================================
// SHOW EVERYONE (fit bounds)
// =========================================================
function fitToEveryone() {
  const pts = [];
  for (const ffId of FF_ORDER) {
    const m = state.members[ffId];
    if (m && isNum(m.lat) && isNum(m.lon)) pts.push([m.lat, m.lon]);
  }
  if (state.incident && isNum(state.incident.lat) && isNum(state.incident.lon)) {
    pts.push([state.incident.lat, state.incident.lon]);
  }
  if (!pts.length) return;

  if (pts.length === 1) { map.setView(pts[0], 19); return; }
  map.fitBounds(L.latLngBounds(pts), { padding: [30, 30], maxZoom: 19 });
}

// =========================================================
// KPI CLASS
// =========================================================
function kpiClass(level) {
  if (level === "Critical" || level === "Danger" || level === "Extreme") return "danger";
  if (level === "High" || level === "Hot") return "warn";
  return "";
}

// =========================================================
// CAMERA HTML (STATIC — inserted ONCE)
// =========================================================
function cameraHtml(ffId) {
  const cam = CAMERA_URLS[ffId];
  if (!cam) return `<div class="cam cam-empty">No camera</div>`;

  if (cam.type === "youtube") {
    return `
      <div class="cam cam-yt" data-ff="${ffId}">
        <div class="yt-host" id="yt-${ffId}"></div>
      </div>
    `;
  }

  // ✅ Demo-safe tile: no embedded reader.js, no /whep spam
  if (cam.type === "external") {
    return `
      <div class="cam cam-empty" data-ff="${ffId}" style="display:flex;align-items:center;justify-content:center;flex-direction:column;gap:10px;">
        <div style="font-weight:900;">LIVE CAMERA</div>
        <div style="opacity:.85;font-size:13px;">Click to open feed</div>
      </div>
    `;
  }

  return `<div class="cam cam-empty">No camera</div>`;
}

// =========================================================
// STABLE CARD RENDER
// =========================================================
function initCardOnce(ffId) {
  const el = document.getElementById(`card-${ffId}`);
  if (!el || el.dataset.inited) return;

  el.dataset.inited = "1";
  el.innerHTML = `
    <div class="unit-top">
      <div class="unit-name" id="name-${ffId}">${ffId}</div>
      <span class="pill stale" id="pill-${ffId}">WAITING</span>
    </div>

    <div class="unit-meta" id="meta-${ffId}">
      <span>${ffId}</span>
      <span>⏱ —</span>
      <span>📍 —</span>
    </div>

    <div class="cam-slot" id="cam-slot-${ffId}">
      ${cameraHtml(ffId)}
    </div>

    <div class="kpis">
      <div class="kpi" id="kpi-pulse-${ffId}">
        <div class="v" id="pulse-${ffId}">—</div><div class="l">Pulse</div>
      </div>
      <div class="kpi" id="kpi-temp-${ffId}">
        <div class="v" id="temp-${ffId}">—</div><div class="l">Temp</div>
      </div>
      <div class="kpi" id="kpi-smoke-${ffId}">
        <div class="v" id="smoke-${ffId}">—</div><div class="l">Smoke</div>
      </div>
    </div>
  `;

  el.addEventListener("click", (ev) => {
    const camEl = ev.target?.closest?.(".cam");
    if (camEl) return;
    const m2 = state.members[ffId];
    if (m2 && isNum(m2.lat) && isNum(m2.lon)) {
      map.setView([m2.lat, m2.lon], 19);
      markers[ffId]?.openPopup();
    }
  });

  const cam = el.querySelector(".cam");
  if (cam) {
    cam.addEventListener("click", (e) => {
      e.stopPropagation();
      openCamera(ffId);
    });
  }
}

function updateCard(ffId) {
  initCardOnce(ffId);

  const m = state.members[ffId];

  const nameEl = document.getElementById(`name-${ffId}`);
  const pillEl = document.getElementById(`pill-${ffId}`);
  const metaEl = document.getElementById(`meta-${ffId}`);

  const pulseEl = document.getElementById(`pulse-${ffId}`);
  const tempEl = document.getElementById(`temp-${ffId}`);
  const smokeEl = document.getElementById(`smoke-${ffId}`);

  const kpiPulse = document.getElementById(`kpi-pulse-${ffId}`);
  const kpiTemp = document.getElementById(`kpi-temp-${ffId}`);
  const kpiSmoke = document.getElementById(`kpi-smoke-${ffId}`);

  if (!m) {
    if (nameEl) nameEl.textContent = ffId;
    if (pillEl) { pillEl.className = "pill stale"; pillEl.textContent = "WAITING"; }
    if (metaEl) metaEl.innerHTML = `<span>${ffId}</span><span>⏱ —</span><span>📍 —</span>`;
    if (pulseEl) pulseEl.textContent = "—";
    if (tempEl) tempEl.textContent = "—";
    if (smokeEl) smokeEl.textContent = "—";
    if (kpiPulse) kpiPulse.className = "kpi";
    if (kpiTemp) kpiTemp.className = "kpi";
    if (kpiSmoke) kpiSmoke.className = "kpi";
    return;
  }

  const stale = (m.lastSeenSec ?? 9999) > 12;
  if (nameEl) nameEl.textContent = m.name || ffId;

  if (pillEl) {
    pillEl.className = stale ? "pill stale" : `pill ${m.severity || "ok"}`;
    pillEl.textContent = stale ? "STALE" : (m.severity || "ok").toUpperCase();
  }

  const dist = isNum(m.distanceToLeaderM) ? `${Math.round(m.distanceToLeaderM)}m` : "—";
  const last = (m.lastSeenSec ?? null) === null ? "—" : `${m.lastSeenSec}s`;
  if (metaEl) metaEl.innerHTML = `<span>${m.ffId}</span><span>⏱ ${last}</span><span>📍 ${dist}</span>`;

  if (pulseEl) pulseEl.textContent = isNum(m.pulse) ? `${Math.round(m.pulse)}` : "—";
  if (tempEl) tempEl.textContent = isNum(m.temp) ? `${m.temp.toFixed(1)}°C` : "—";
  if (smokeEl) smokeEl.textContent = isNum(m.gas) ? `${Math.round(m.gas)}` : "—";

  if (kpiPulse) kpiPulse.className = `kpi ${kpiClass(m.pulseLevel)}`;
  if (kpiTemp) kpiTemp.className = `kpi ${kpiClass(m.heatLevel)}`;
  if (kpiSmoke) kpiSmoke.className = `kpi ${kpiClass(m.smokeLevel)}`;
}

function renderAllCards() {
  FF_ORDER.forEach(updateCard);
}

// =========================================================
// ALERTS
// =========================================================
function worstOverall() {
  let w = "ok";
  for (const p of Object.values(state.alertsByFf)) {
    if (!p) continue;
    if (sevRank(p.worst) > sevRank(w)) w = p.worst;
  }
  return w;
}

function updateMissionBadge() {
  const worst = worstOverall();
  const badge = document.getElementById("missionBadge");
  const panel = document.getElementById("alertsPanel");
  const sub = document.getElementById("alertsSub");

  if (badge) {
    badge.classList.remove("ok", "warn", "danger");
    badge.classList.add(worst);
    badge.textContent = worst === "danger" ? "Danger" : worst === "warn" ? "Warning" : "Nominal";
  }
  if (panel) {
    panel.classList.remove("ok", "warn", "danger");
    panel.classList.add(worst);
  }
  if (sub) {
    sub.textContent =
      worst === "danger" ? "Immediate attention required" :
      worst === "warn" ? "Monitor active risks" :
      "No active alerts";
  }
}

function renderAlerts() {
  const content = document.getElementById("alertContent");
  if (!content) return;

  const packets = Object.values(state.alertsByFf)
    .filter(p => p && Array.isArray(p.alerts) && p.alerts.length)
    .sort((a, b) => sevRank(b.worst) - sevRank(a.worst));

  content.innerHTML = "";

  if (!packets.length) {
    content.innerHTML = `<div class="empty">No active alerts.</div>`;
    updateMissionBadge();
    return;
  }

  packets.slice(0, 2).forEach(p => {
    const block = document.createElement("div");
    block.className = "alert-block";

    block.innerHTML = `
      <div class="alert-head2">
        <div class="alert-name">${p.name || p.ffId}</div>
        <span class="pill ${p.worst}">${p.worst.toUpperCase()}</span>
      </div>
      <div class="alert-meta">Active conditions</div>
    `;

    p.alerts.slice(0, 4).forEach(a => {
      const item = document.createElement("div");
      item.className = `alert-item ${a.severity}`;
      item.innerHTML = `<div class="t">${a.title}</div><div class="d">${a.detail}</div>`;
      block.appendChild(item);
    });

    content.appendChild(block);
  });

  updateMissionBadge();
}

// =========================================================
// YOUTUBE IFRAME API — single load only
// =========================================================
let ytReady = false;
const ytPlayers = {};
const ytLoopTimers = {};

function loadYouTubeAPI() {
  if (window.YT && window.YT.Player) {
    ytReady = true;
    initAllYouTubePlayers();
    return;
  }

  // avoid duplicates
  if (document.getElementById("yt-api")) return;

  const s = document.createElement("script");
  s.id = "yt-api";
  s.src = "https://www.youtube.com/iframe_api";
  document.head.appendChild(s);

  window.onYouTubeIframeAPIReady = () => {
    ytReady = true;
    initAllYouTubePlayers();
  };
}

function ensureYTPlayer(ffId) {
  const cam = CAMERA_URLS[ffId];
  if (!cam || cam.type !== "youtube") return;
  if (!ytReady) return;
  if (ytPlayers[ffId]) return;

  const hostId = `yt-${ffId}`;
  const host = document.getElementById(hostId);
  if (!host) return;

  const start = cam.start ?? 0;
  const end = start + (cam.dur ?? 40);

  ytPlayers[ffId] = new YT.Player(hostId, {
    width: "100%",
    height: "100%",
    videoId: cam.videoId,
    playerVars: {
      autoplay: 1,
      controls: 0,
      mute: 1,
      playsinline: 1,
      rel: 0,
      modestbranding: 1,
      start,
      origin: location.origin, // reduces postMessage origin issues
    },
    events: {
      onReady: (e) => {
        try {
          e.target.mute();
          e.target.seekTo(start, true);
          e.target.playVideo();
        } catch {}

        if (ytLoopTimers[ffId]) clearInterval(ytLoopTimers[ffId]);
        ytLoopTimers[ffId] = setInterval(() => {
          const p = ytPlayers[ffId];
          if (!p || typeof p.getCurrentTime !== "function") return;
          const t = p.getCurrentTime();
          if (t >= end - 0.2) {
            try { p.seekTo(start, true); p.playVideo(); } catch {}
          }
        }, 250);
      },
      onStateChange: (e) => {
        if (e.data === 2 || e.data === 3) {
          try { e.target.playVideo(); } catch {}
        }
      }
    }
  });
}

function initAllYouTubePlayers() {
  renderAllCards();
  ["FF_B", "FF_C", "FF_D"].forEach(ensureYTPlayer);
}

// =========================================================
// MQTT
// =========================================================
function connectMQTT() {
  const client = mqtt.connect(`ws://${CONFIG.MQTT_HOST}:${CONFIG.MQTT_PORT}/mqtt`);

  client.on("connect", () => {
    const dot = document.querySelector(".dot");
    const text = document.querySelector(".conn-text");
    if (dot) dot.classList.add("on");
    if (text) text.textContent = "MQTT Online";

    client.subscribe(CONFIG.TEAM_TOPIC);
    client.subscribe(CONFIG.ALERT_TOPIC);
    client.subscribe(CONFIG.INCIDENT_TOPIC);
  });

  client.on("message", (topic, payload) => {
    let msg;
    try { msg = JSON.parse(payload.toString()); } catch { return; }

    if (topic.includes("edge/status")) {
      if (!msg.ffId) return;

      state.members[msg.ffId] = msg;
      upsertMarker(msg);

      scheduleRender();
      maybeCenterOnLeader();

      if (msg.ffId === "FF_B" || msg.ffId === "FF_C" || msg.ffId === "FF_D") {
        ensureYTPlayer(msg.ffId);
      }
    }

    if (topic.includes("edge/alerts")) {
      if (!msg.ffId) return;
      state.alertsByFf[msg.ffId] = msg;
      renderAlerts();
    }

    if (topic === CONFIG.INCIDENT_TOPIC) {
      if (!isNum(msg.lat) || !isNum(msg.lon) || !isNum(msg.radiusM)) return;
      updateIncidentUI(msg);
    }
  });
}

// =========================================================
// INIT
// =========================================================
function initMap() {
  map = L.map("mainMap").setView([38.2466, 21.7346], 19);
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
    maxZoom: 20,
    attribution: "© OpenStreetMap"
  }).addTo(map);

  const btn = document.getElementById("btnRecenter");
  if (btn) {
    btn.onclick = () => {
      const a = state.members["FF_A"];
      if (a && isNum(a.lat) && isNum(a.lon)) map.setView([a.lat, a.lon], 19);
    };
  }

  const btnAll = document.getElementById("btnShowAll");
  if (btnAll) btnAll.onclick = () => fitToEveryone();

  wirePopupOnce();
  renderAllCards();
  loadYouTubeAPI();
  renderAlerts();
}

function boot() {
  // If libs are deferred, wait until they exist
  if (!window.L || !window.mqtt) {
    setTimeout(boot, 50);
    return;
  }
  initMap();
  connectMQTT();
  startCommandActionPolling();
  console.log("✅ app.js boot complete");
}

boot();
