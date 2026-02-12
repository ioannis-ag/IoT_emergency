/* =========================================================
   Command Center Frontend (Always show FF_A..FF_D)

   ✅ Map
   - Markers always render using last-known coords
   - Trails: keep last 10 points; append only when coords change enough
   - FIX: if FF_A has NO lat/lon in /api/latest, we fallback to:
       1) lastPos from previous runs (localStorage)
       2) average of other FF coords (B/C/D) this tick (so leader still appears)

   ✅ Incident circle
   - Small circle near firefighters
   - IMPORTANT: circle is click-through (interactive:false) so markers remain clickable
   - Also placed in a dedicated pane behind markers

   ✅ Grafana
   - HR + TEMP (panel 1..4 HR, panel 5..8 TEMP)
   - refresh=5s so it actually refreshes

   ✅ Weather
   - /api/weather uses leader coords; fallback to Patras coords if leader missing

   ✅ Video in Team tab (Operator Metrics)
   - FF_A: mtxmedia iframe 192.168.2.13:8889/cam
   - FF_B/C/D: YouTube embed (nocookie) starting at 10s / 70s / 130s
     * Loops every 60s by reloading iframe with cache-buster

   ========================================================= */

const CONFIG = {
  TEAM: "Team_A",
  POLL_MS: 2000,
  LATEST_MINUTES: 60,

  WEATHER_POLL_MS: 5 * 60 * 1000,
  WEATHER_FALLBACK: { lat: 38.2466, lon: 21.7346, label: "Patras fallback" },

  TRAIL_POINTS: 10,
  TRAIL_MIN_METERS: 2,

  ACTIONS_POLL_MS: 4000,
  ACTIONS_MINUTES: 180,

  FETCH_TIMEOUT_MS: 4500,

  // Incident circle (≈ 3–4 buildings)
  INCIDENT_RADIUS_M: 30,
  INCIDENT_FOLLOW: true, // keep it near the team/leader
};

const GRAFANA = {
  BASE: "http://192.168.2.12:8087",
  ORG_ID: 1,
  DASH_UID: "ad5htqk",
  DASH_SLUG: "command-center-dashboard",
  FROM: "now-5m",
  TO: "now",
  THEME: "light",
  REFRESH: "5s",
  HR_PANEL_BY_FF: { FF_A: 1, FF_B: 2, FF_C: 3, FF_D: 4 },
  TEMP_PANEL_BY_FF: { FF_A: 5, FF_B: 6, FF_C: 7, FF_D: 8 },
};

const VIDEO = {
  FF_A: { type: "iframe", url: "http://192.168.2.13:8889/cam" },

  YT_VIDEO_ID: "mphHFk5IXsQ",
  SEG_SECONDS: 60,
  START_BY_FF: { FF_B: 10, FF_C: 70, FF_D: 130 },
};

const FF_NAMES = {
  FF_A: "Alex (Leader)",
  FF_B: "Maria",
  FF_C: "Nikos",
  FF_D: "Eleni",
};

const FF_FIXED = ["FF_A", "FF_B", "FF_C", "FF_D"];

const state = {
  page: "map",
  selectedFF: "FF_A",
  latestByFf: {},

  polling: false,
  mapCenteredOnce: false,
  lastWeatherAt: 0,

  actionLog: [],
  alertsCache: [],
  lastActionsPollAt: 0,

  // last known position per FF (persist forever)
  lastPos: { FF_A: null, FF_B: null, FF_C: null, FF_D: null },

  grafana: { lastFf: null, lastHrSrc: "", lastTempSrc: "" },

  video: { lastFf: null, lastSrc: "", loopTimer: 0 },

  incident: { lat: null, lon: null, lastSetAt: 0 },
};

/* ---------------- Helpers ---------------- */

function safeCall(fn, ...args) {
  try {
    if (typeof fn === "function") return fn(...args);
  } catch (e) {
    console.warn(e);
  }
}

function escapeHtml(s) {
  return String(s ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function flashToast(msg, ms = 1800) {
  try {
    let el = document.getElementById("toast");
    if (!el) {
      el = document.createElement("div");
      el.id = "toast";
      el.style.position = "fixed";
      el.style.left = "50%";
      el.style.bottom = "18px";
      el.style.transform = "translateX(-50%)";
      el.style.zIndex = "9999";
      el.style.background = "rgba(15,23,42,.92)";
      el.style.color = "white";
      el.style.padding = "10px 12px";
      el.style.borderRadius = "14px";
      el.style.fontSize = "13px";
      el.style.fontWeight = "900";
      el.style.boxShadow = "0 10px 22px rgba(0,0,0,.25)";
      el.style.opacity = "0";
      el.style.transition = "opacity .15s";
      document.body.appendChild(el);
    }
    el.textContent = msg;
    el.style.opacity = "1";
    clearTimeout(el._t);
    el._t = setTimeout(() => (el.style.opacity = "0"), ms);
  } catch (_) {}
}

function isNum(x) {
  return typeof x === "number" && Number.isFinite(x);
}
function fmt(x, d = 0) {
  return isNum(x) ? x.toFixed(d) : "—";
}

function toNum(x) {
  if (x === null || x === undefined) return null;
  if (typeof x === "number") return Number.isFinite(x) ? x : null;
  if (typeof x === "string") {
    const n = Number(x);
    return Number.isFinite(n) ? n : null;
  }
  return null;
}

function normId(x) {
  return x == null ? null : String(x).trim();
}

function canonicalId(id) {
  const s = normId(id);
  if (!s) return null;
  const parts = s.split("/").filter(Boolean);
  return (parts[parts.length - 1] || s).trim();
}

/**
 * Robust coordinate extractor.
 * Supports:
 *  - m.lat/m.lon
 *  - m.lat/m.lng
 *  - m.location.lat/m.location.lon(lng)
 *  - m.location.coordinates [lon,lat]
 *  - m.position.lat/m.position.lon(lng)
 *  - m.position.coordinates [lon,lat]
 */
function extractCoords(m) {
  if (!m) return { lat: null, lon: null };

  let lat = toNum(m.lat);
  let lon = toNum(m.lon ?? m.lng);

  if ((!isNum(lat) || !isNum(lon)) && m.location) {
    const L = m.location;
    const lat2 = toNum(L.lat);
    const lon2 = toNum(L.lon ?? L.lng);
    if (isNum(lat2) && isNum(lon2)) {
      lat = lat2;
      lon = lon2;
    } else if (Array.isArray(L.coordinates) && L.coordinates.length >= 2) {
      const lonG = toNum(L.coordinates[0]);
      const latG = toNum(L.coordinates[1]);
      if (isNum(latG) && isNum(lonG)) {
        lat = latG;
        lon = lonG;
      }
    }
  }

  if ((!isNum(lat) || !isNum(lon)) && m.position) {
    const P = m.position;
    const lat2 = toNum(P.lat);
    const lon2 = toNum(P.lon ?? P.lng);
    if (isNum(lat2) && isNum(lon2)) {
      lat = lat2;
      lon = lon2;
    } else if (Array.isArray(P.coordinates) && P.coordinates.length >= 2) {
      const lonG = toNum(P.coordinates[0]);
      const latG = toNum(P.coordinates[1]);
      if (isNum(latG) && isNum(lonG)) {
        lat = latG;
        lon = lonG;
      }
    }
  }

  return { lat, lon };
}

function setConn(ok, text) {
  const dot = document.querySelector("#connectionStatus .dot") || document.querySelector(".dot");
  const t =
    document.querySelector("#connectionStatus .conn-text") || document.querySelector(".conn-text");
  if (dot) dot.classList.toggle("on", !!ok);
  if (t) t.textContent = text;
}

function setBadge(el, sev, text) {
  if (!el) return;
  el.classList.remove("ok", "warn", "danger");
  el.classList.add(sev);
  if (text != null) el.textContent = text;
}

function tickClock() {
  const el = document.getElementById("clock");
  if (!el) return;
  el.textContent = new Date().toLocaleString(undefined, {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    day: "2-digit",
    month: "short",
    year: "numeric",
  });
}

/* ---------------- Persistence (lastPos) ---------------- */

function loadLastPos() {
  try {
    const raw = localStorage.getItem("cc_last_pos");
    if (!raw) return;
    const obj = JSON.parse(raw);
    if (obj && typeof obj === "object") {
      for (const ff of FF_FIXED) {
        const p = obj[ff];
        if (p && isNum(p.lat) && isNum(p.lon)) state.lastPos[ff] = { lat: p.lat, lon: p.lon, ts: p.ts || 0 };
      }
    }
  } catch (_) {}
}

function saveLastPos() {
  try {
    const out = {};
    for (const ff of FF_FIXED) {
      const p = state.lastPos[ff];
      if (p && isNum(p.lat) && isNum(p.lon)) out[ff] = { lat: p.lat, lon: p.lon, ts: p.ts || 0 };
    }
    localStorage.setItem("cc_last_pos", JSON.stringify(out));
  } catch (_) {}
}

/* ---------------- Tabs ---------------- */

function showPage(page) {
  state.page = page;

  document.querySelectorAll(".tab-btn").forEach((b) =>
    b.classList.toggle("active", b.dataset.page === page)
  );

  document.querySelectorAll(".page").forEach((sec) => {
    const active = sec.id === `page-${page}`;
    sec.classList.toggle("active", active);
    sec.style.display = active ? "block" : "none";
  });

  if (page === "map" && window._map) setTimeout(() => window._map.invalidateSize(), 150);

  if (page === "team") safeCall(updateTeamPage);
  if (page === "risk") safeCall(updateRiskPage);
  if (page === "alerts") safeCall(updateAlertsPage);
  if (page === "medical") safeCall(updateMedicalPage);
}

function initTabs() {
  document.querySelectorAll(".tab-btn").forEach((btn) =>
    btn.addEventListener("click", () => showPage(btn.dataset.page))
  );

  document.querySelectorAll(".subtab").forEach((btn) => {
    btn.addEventListener("click", () => {
      state.selectedFF = btn.dataset.ff;

      document.querySelectorAll(".subtab").forEach((b) =>
        b.classList.toggle("active", b.dataset.ff === state.selectedFF)
      );

      updateGrafanaFrames(true);
      updateTeamVideo(true);

      if (state.page === "team") safeCall(updateTeamPage);
      if (state.page === "risk") safeCall(updateRiskPage);
    });
  });
}

/* ---------------- Severity logic ---------------- */

function computeSeverity(m) {
  let sev = "ok";
  if (
    (isNum(m.hrBpm) && m.hrBpm >= 150) ||
    (isNum(m.tempC) && m.tempC >= 50) ||
    (isNum(m.mq2Raw) && m.mq2Raw >= 2600) ||
    (isNum(m.stressIndex) && m.stressIndex >= 0.75)
  )
    sev = "warn";

  if (
    (isNum(m.hrBpm) && m.hrBpm >= 170) ||
    (isNum(m.tempC) && m.tempC >= 60) ||
    (isNum(m.mq2Raw) && m.mq2Raw >= 3200) ||
    (isNum(m.stressIndex) && m.stressIndex >= 0.9)
  )
    sev = "danger";

  return sev;
}

function sevColor(sev) {
  if (sev === "danger") return "#ef4444";
  if (sev === "warn") return "#f59e0b";
  return "#16a34a";
}

/* ---------------- Trail helpers ---------------- */

function distanceMeters(a, b) {
  const R = 6371000;
  const toRad = (x) => (x * Math.PI) / 180;
  const dLat = toRad(b[0] - a[0]);
  const dLon = toRad(b[1] - a[1]);
  const lat1 = toRad(a[0]);
  const lat2 = toRad(b[0]);
  const s =
    Math.sin(dLat / 2) ** 2 +
    Math.cos(lat1) * Math.cos(lat2) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(s)));
}

/* ---------------- Map ---------------- */

function initMap() {
  const el = document.getElementById("mainMap");
  if (!el) return;

  const map = L.map("mainMap").setView([38.2466, 21.7346], 18);

  // ✅ Pane ordering:
  // incidentPane behind everything
  map.createPane("incidentPane");
  map.getPane("incidentPane").style.zIndex = 350; // tiles=200, overlays~400, markers~600

  // trails/lines normal overlayPane (default zIndex 400)
  // markers use markerPane (600) automatically

  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
    maxZoom: 20,
    attribution: "© OpenStreetMap",
  }).addTo(map);

  window._map = map;
  window._markers = {};
  window._tracks = {};
  window._trackLines = {};
  window._incidentCircle = null;

  document
    .getElementById("btnRecenterLeader")
    ?.addEventListener("click", () => centerOnLeader(true));
}

function ensureTrack(ffId) {
  if (!window._tracks[ffId]) window._tracks[ffId] = [];
  if (!window._trackLines[ffId] && window._map) {
    window._trackLines[ffId] = L.polyline([], { weight: 3, opacity: 0.75 }).addTo(window._map);
  }
}

function appendTrailPoint(ffId, lat, lon) {
  if (!window._map) return;
  ensureTrack(ffId);

  const pts = window._tracks[ffId];
  const p = [lat, lon];
  const last = pts.length ? pts[pts.length - 1] : null;

  if (last && distanceMeters(last, p) < CONFIG.TRAIL_MIN_METERS) return;

  pts.push(p);
  if (pts.length > CONFIG.TRAIL_POINTS) pts.splice(0, pts.length - CONFIG.TRAIL_POINTS);
  window._trackLines[ffId].setLatLngs(pts);
}

function popupHtml(ffId, m, lat, lon) {
  return `
    <div style="min-width:220px">
      <div style="font-weight:1100">${escapeHtml(FF_NAMES[ffId] || ffId)}
        <span style="color:#54657e;font-weight:900">(${escapeHtml(ffId)})</span>
      </div>
      <div style="margin-top:6px;color:#54657e;font-size:13px;line-height:1.45">
        📍 ${isNum(lat) ? lat.toFixed(6) : "—"}, ${isNum(lon) ? lon.toFixed(6) : "—"}<br/>
        🕒 ${escapeHtml(m?.observedAt || "—")}<br/>
        ❤️ HR: <b>${fmt(m?.hrBpm, 0)}</b><br/>
        🌡️ Temp: <b>${fmt(m?.tempC, 1)}°C</b><br/>
        🫁 Smoke: <b>${fmt(m?.mq2Raw, 0)}</b><br/>
        🧠 Stress: <b>${fmt(m?.stressIndex, 2)}</b>
      </div>
    </div>
  `;
}

/**
 * ✅ FIX for FF_A missing lat/lon in /api/latest:
 * If FF_A has no coords and has never had coords, fallback to average of others this tick.
 */
function getFallbackLeaderFromOthers() {
  const pts = [];
  for (const ff of ["FF_B", "FF_C", "FF_D"]) {
    const p = state.lastPos[ff];
    if (p && isNum(p.lat) && isNum(p.lon)) pts.push([p.lat, p.lon]);
  }
  if (!pts.length) return null;
  const lat = pts.reduce((a, b) => a + b[0], 0) / pts.length;
  const lon = pts.reduce((a, b) => a + b[1], 0) / pts.length;
  return { lat, lon };
}

/**
 * ALWAYS show marker if we have any last-known coordinates.
 * If current coords missing, use lastPos.
 * If still missing and ffId is FF_A, use fallback from others (so leader appears).
 */
function upsertMarkerAlways(ffId, m) {
  if (!window._map) return;

  const c = extractCoords(m);
  const hadFresh = isNum(c.lat) && isNum(c.lon);

  let lat = c.lat,
    lon = c.lon;

  if (!hadFresh) {
    const last = state.lastPos[ffId];
    if (last && isNum(last.lat) && isNum(last.lon)) {
      lat = last.lat;
      lon = last.lon;
    } else if (ffId === "FF_A") {
      const fb = getFallbackLeaderFromOthers();
      if (fb) {
        lat = fb.lat;
        lon = fb.lon;
        // store so center/weather/incident can work
        state.lastPos["FF_A"] = { lat, lon, ts: Date.now() };
      }
    }
  } else {
    state.lastPos[ffId] = { lat, lon, ts: Date.now() };
    appendTrailPoint(ffId, lat, lon);
  }

  if (!isNum(lat) || !isNum(lon)) return;

  const sev = computeSeverity(m || {});
  const color = sevColor(sev);
  const popup = popupHtml(ffId, m || {}, lat, lon);

  const store = window._markers;

  if (!store[ffId]) {
    const mk = L.circleMarker([lat, lon], {
      radius: 7,
      color: "#ffffff",
      weight: 2,
      fillColor: color,
      fillOpacity: 0.95,
      opacity: 1,
    }).addTo(window._map);

    mk.bindPopup(popup);
    mk.bindTooltip(FF_NAMES[ffId] || ffId, {
      direction: "top",
      offset: [0, -10],
      opacity: 0.9,
    });

    store[ffId] = mk;
  } else {
    store[ffId].setLatLng([lat, lon]);
    store[ffId].setStyle({ fillColor: color, color: "#ffffff" });
    try {
      store[ffId].getPopup()?.setContent(popup);
    } catch (_) {}
  }
}

function centerOnLeader(force = false) {
  if (!window._map) return;

  const p = state.lastPos["FF_A"];
  if (p && isNum(p.lat) && isNum(p.lon)) {
    if (force || !state.mapCenteredOnce) {
      window._map.setView([p.lat, p.lon], 18);
      state.mapCenteredOnce = true;
    }
    return;
  }

  const any = Object.values(state.lastPos).find((v) => v && isNum(v.lat) && isNum(v.lon));
  if (any && (force || !state.mapCenteredOnce)) {
    window._map.setView([any.lat, any.lon], 18);
    state.mapCenteredOnce = true;
  }
}

/* ---------------- Incident circle ---------------- */

function computeIncidentCenter() {
  // Prefer leader; else average of any available
  const leader = state.lastPos["FF_A"];
  if (leader && isNum(leader.lat) && isNum(leader.lon)) return { lat: leader.lat, lon: leader.lon };

  const pts = [];
  for (const ff of FF_FIXED) {
    const p = state.lastPos[ff];
    if (p && isNum(p.lat) && isNum(p.lon)) pts.push([p.lat, p.lon]);
  }
  if (!pts.length) return null;

  const lat = pts.reduce((a, b) => a + b[0], 0) / pts.length;
  const lon = pts.reduce((a, b) => a + b[1], 0) / pts.length;
  return { lat, lon };
}

function updateIncidentCircle() {
  if (!window._map) return;

  const c = computeIncidentCenter();
  if (!c) return;

  // keep it "near" but not exactly on top (small offset)
  const lat = c.lat + 0.00012;
  const lon = c.lon + 0.00010;

  // If not following, only set once
  if (!CONFIG.INCIDENT_FOLLOW && window._incidentCircle) return;

  if (!window._incidentCircle) {
    window._incidentCircle = L.circle([lat, lon], {
      radius: CONFIG.INCIDENT_RADIUS_M,
      color: "#ef4444",
      weight: 2,
      opacity: 0.9,
      fillColor: "#ef4444",
      fillOpacity: 0.12,

      // ✅ critical: click-through so firefighters remain clickable
      interactive: false,

      // ✅ keep it behind markers
      pane: "incidentPane",
    }).addTo(window._map);
  } else {
    window._incidentCircle.setLatLng([lat, lon]);
    window._incidentCircle.setRadius(CONFIG.INCIDENT_RADIUS_M);
  }
}

/* ---------------- API (with timeout) ---------------- */

async function fetchJson(path, timeoutMs = CONFIG.FETCH_TIMEOUT_MS) {
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const r = await fetch(path, { cache: "no-store", signal: ctrl.signal });
    if (!r.ok) throw new Error(`${r.status} ${path}`);
    return await r.json();
  } finally {
    clearTimeout(t);
  }
}

const apiHealth = () => fetchJson("/api/health");
const apiLatest = () =>
  fetchJson(`/api/latest?team=${encodeURIComponent(CONFIG.TEAM)}&minutes=${CONFIG.LATEST_MINUTES}`);
const apiWeather = (lat, lon) =>
  fetchJson(`/api/weather?lat=${encodeURIComponent(lat)}&lon=${encodeURIComponent(lon)}`, 6500);
const apiAlerts = (minutes = 180) =>
  fetchJson(`/api/alerts?team=${encodeURIComponent(CONFIG.TEAM)}&minutes=${minutes}`);
const apiActions = (minutes = 180) =>
  fetchJson(`/api/actions?team=${encodeURIComponent(CONFIG.TEAM)}&minutes=${minutes}`);

async function apiAction(payload) {
  const r = await fetch("/api/action", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload || {}),
  });
  if (!r.ok) throw new Error(`${r.status} /api/action`);
  return await r.json();
}

/* ---------------- Action log (local) ---------------- */

function loadActionLog() {
  try {
    const raw = localStorage.getItem("cc_action_log");
    const arr = raw ? JSON.parse(raw) : [];
    if (Array.isArray(arr)) state.actionLog = arr;
  } catch (_) {}
}
function pushActionLog(item) {
  try {
    state.actionLog.unshift(item);
    state.actionLog = state.actionLog.slice(0, 500);
    localStorage.setItem("cc_action_log", JSON.stringify(state.actionLog));
  } catch (_) {}
}

function addActionsFromServer(actionsArr) {
  if (!Array.isArray(actionsArr) || !actionsArr.length) return;

  const seen = new Set((state.actionLog || []).map((x) => x?.id).filter(Boolean));
  const incoming = [];

  for (const a of actionsArr) {
    const id = a?.id || "";
    if (id && seen.has(id)) continue;

    incoming.push({
      type: "action",
      id,
      action: a?.action || "ACTION",
      teamId: a?.teamId || CONFIG.TEAM,
      ffId: a?.ffId || "ALL",
      note: a?.note || "",
      observedAt: a?.observedAt || a?.time || new Date().toISOString(),
    });
  }

  if (incoming.length) {
    incoming.sort((x, y) => String(y.observedAt || "").localeCompare(String(x.observedAt || "")));
    state.actionLog = incoming.concat(state.actionLog || []).slice(0, 500);
    try {
      localStorage.setItem("cc_action_log", JSON.stringify(state.actionLog));
    } catch (_) {}
  }
}

/* ---------------- Dashboard UI ---------------- */

function updateDashboardTiles() {
  setConn(true, "API Mode");
  const have = Object.keys(state.latestByFf).length;
  const subA = document.getElementById("tileSubA");
  if (subA) subA.textContent = `${have}/4 updating`;
  const meta = document.getElementById("teamsMeta");
  if (meta) meta.textContent = `Selected: ${CONFIG.TEAM}`;
}

function updateTeamStatusPanel() {
  let worst = "ok",
    worstFf = null;
  for (const [ffId, m] of Object.entries(state.latestByFf)) {
    const s = computeSeverity(m || {});
    if (s === "danger") {
      worst = "danger";
      worstFf = ffId;
      break;
    }
    if (s === "warn" && worst !== "danger") {
      worst = "warn";
      worstFf = ffId;
    }
  }

  const badge = document.getElementById("teamStatusBadge");
  const box = document.getElementById("teamStatusBox");

  if (worst === "danger") {
    setBadge(badge, "danger", "DANGER");
    if (box)
      box.innerHTML = `<div class="status-title">Critical Condition</div><div class="status-desc">Highest risk: ${
        escapeHtml(FF_NAMES[worstFf] || worstFf || "—")
      }</div>`;
  } else if (worst === "warn") {
    setBadge(badge, "warn", "WARNING");
    if (box)
      box.innerHTML = `<div class="status-title">Warning Detected</div><div class="status-desc">Watch: ${
        escapeHtml(FF_NAMES[worstFf] || worstFf || "—")
      }</div>`;
  } else {
    setBadge(badge, "ok", "NOMINAL");
    if (box)
      box.innerHTML = `<div class="status-title">All Systems Nominal</div><div class="status-desc">No active critical conditions detected.</div>`;
  }
}

/* ---------------- Weather ---------------- */

function pickNum(obj, keys) {
  for (const k of keys) {
    const v = toNum(obj?.[k]);
    if (isNum(v)) return v;
  }
  return null;
}

function updateWeatherUI(w, meta = {}) {
  const tEl = document.getElementById("wxTemp");
  const wEl = document.getElementById("wxWind");
  const hEl = document.getElementById("wxHum");

  const tempC = pickNum(w, ["tempC", "temperatureC", "temperature", "temp"]);
  const windMs = pickNum(w, ["windMs", "wind_m_s", "windSpeedMs", "windSpeed", "wind"]);
  const humPct = pickNum(w, ["humidityPct", "humidity", "rh", "relativeHumidity"]);

  if (tEl) tEl.textContent = isNum(tempC) ? `${fmt(tempC, 1)}°C` : "—";
  if (wEl) wEl.textContent = isNum(windMs) ? `${fmt(windMs, 1)} m/s` : "—";
  if (hEl) hEl.textContent = isNum(humPct) ? `${fmt(humPct, 0)}%` : "—";

  const riskRaw = String(w?.risk ?? w?.riskLevel ?? w?.fireRisk ?? "LOW").toUpperCase();
  const badge = document.getElementById("weatherBadge");
  if (riskRaw === "HIGH" || riskRaw === "EXTREME") setBadge(badge, "danger", riskRaw);
  else if (riskRaw === "MEDIUM" || riskRaw === "MODERATE")
    setBadge(badge, "warn", riskRaw === "MODERATE" ? "MEDIUM" : riskRaw);
  else setBadge(badge, "ok", riskRaw);

  const hint = document.getElementById("wxHint");
  if (hint) {
    const src = w?.source ? ` (${w.source})` : "";
    const where = meta?.where ? ` • ${meta.where}` : "";
    hint.textContent = w?.observedAt
      ? `Weather updated${src}${where}: ${w.observedAt}`
      : `Weather updated${src}${where}.`;
  }
}

function getLeaderCoords() {
  const p = state.lastPos["FF_A"];
  if (p && isNum(p.lat) && isNum(p.lon)) return { lat: p.lat, lon: p.lon };
  return null;
}

async function pollWeatherIfDue() {
  const now = Date.now();
  if (now - state.lastWeatherAt < CONFIG.WEATHER_POLL_MS) return;

  const hint = document.getElementById("wxHint");
  const leader = getLeaderCoords();

  const lat = leader?.lat ?? CONFIG.WEATHER_FALLBACK.lat;
  const lon = leader?.lon ?? CONFIG.WEATHER_FALLBACK.lon;
  const where = leader ? "Leader (FF_A) location" : CONFIG.WEATHER_FALLBACK.label;

  if (!leader && hint) hint.textContent = "Leader location not available yet — using fallback weather location…";

  try {
    const w = await apiWeather(lat, lon);
    updateWeatherUI(w, { where });
    state.lastWeatherAt = now;
  } catch (e) {
    console.warn("weather failed:", e);
    setBadge(document.getElementById("weatherBadge"), "warn", "N/A");
    if (hint) hint.textContent = `Weather fetch failed (${where}). Check /api/weather.`;
    state.lastWeatherAt = now;
  }
}

/* ---------------- Grafana embeds ---------------- */

function buildGrafanaSoloUrl(panelId) {
  const base = `${GRAFANA.BASE}/d-solo/${encodeURIComponent(GRAFANA.DASH_UID)}/${encodeURIComponent(
    GRAFANA.DASH_SLUG
  )}`;
  const params = new URLSearchParams();
  params.set("orgId", String(GRAFANA.ORG_ID));
  params.set("panelId", String(panelId));
  params.set("from", GRAFANA.FROM);
  params.set("to", GRAFANA.TO);
  params.set("theme", GRAFANA.THEME);
  params.set("kiosk", "tv");
  params.set("refresh", GRAFANA.REFRESH);
  return `${base}?${params.toString()}`;
}

function updateGrafanaFrames(force = false) {
  const ff = state.selectedFF;

  const hrFrame = document.getElementById("hrGrafanaFrame");
  const tFrame = document.getElementById("tempGrafanaFrame");

  const hrPanel = GRAFANA.HR_PANEL_BY_FF?.[ff] || 1;
  const tPanel = GRAFANA.TEMP_PANEL_BY_FF?.[ff] || 5;

  const hrSrc = buildGrafanaSoloUrl(hrPanel);
  const tSrc = buildGrafanaSoloUrl(tPanel);

  if (force || state.grafana.lastFf !== ff) {
    if (hrFrame && state.grafana.lastHrSrc !== hrSrc) hrFrame.src = hrSrc;
    if (tFrame && state.grafana.lastTempSrc !== tSrc) tFrame.src = tSrc;

    state.grafana.lastFf = ff;
    state.grafana.lastHrSrc = hrSrc;
    state.grafana.lastTempSrc = tSrc;

    setBadge(document.getElementById("hrPill"), "ok", ff);
    setBadge(document.getElementById("tempPill"), "ok", ff);
  }
}

/* ---------------- Team video (inject UI if missing) ---------------- */

function ensureTeamVideoSlot() {
  // We try to place the video under the Operator Metrics panel, under envCards if possible.
  let frame = document.getElementById("teamVideoFrame");
  let hint = document.getElementById("videoHint");

  if (frame && hint) return { frame, hint };

  const envCards = document.getElementById("envCards");
  const metricsHint = document.getElementById("metricsHint");
  const metricsPanelBody = envCards?.closest(".panel-body");

  if (!metricsPanelBody) return { frame: null, hint: null };

  const wrap = document.createElement("div");
  wrap.style.marginTop = "12px";
  wrap.style.borderTop = "1px solid rgba(15,23,42,.08)";
  wrap.style.paddingTop = "12px";

  const title = document.createElement("div");
  title.textContent = "Livestream";
  title.style.fontWeight = "1100";
  title.style.fontSize = "13px";
  title.style.marginBottom = "8px";
  wrap.appendChild(title);

  frame = document.createElement("iframe");
  frame.id = "teamVideoFrame";
  frame.src = "about:blank";
  frame.loading = "lazy";
  frame.referrerPolicy = "no-referrer";
  frame.allow =
    "autoplay; encrypted-media; picture-in-picture; fullscreen; clipboard-write";
  frame.style.width = "100%";
  frame.style.height = "180px"; // smaller
  frame.style.border = "1px solid rgba(15,23,42,.10)";
  frame.style.borderRadius = "14px";
  frame.style.background = "#fff";
  wrap.appendChild(frame);

  hint = document.createElement("div");
  hint.id = "videoHint";
  hint.textContent = "—";
  hint.style.marginTop = "8px";
  hint.style.color = "var(--muted)";
  hint.style.fontSize = "12px";
  hint.style.fontWeight = "900";
  wrap.appendChild(hint);

  // Insert before metricsHint if present, else append
  if (metricsHint && metricsHint.parentElement === metricsPanelBody) {
    metricsPanelBody.insertBefore(wrap, metricsHint);
  } else {
    metricsPanelBody.appendChild(wrap);
  }

  return { frame, hint };
}

function ytEmbedUrlNocookie(videoId, startSec) {
  const params = new URLSearchParams();
  params.set("autoplay", "1");
  params.set("mute", "1"); // to satisfy autoplay policies
  params.set("controls", "0");
  params.set("rel", "0");
  params.set("modestbranding", "1");
  params.set("playsinline", "1");
  params.set("start", String(Math.max(0, Math.floor(startSec || 0))));

  // loop whole video; we enforce segment looping ourselves with a 60s reload timer
  params.set("loop", "1");
  params.set("playlist", videoId);

  return `https://www.youtube-nocookie.com/embed/${encodeURIComponent(videoId)}?${params.toString()}`;
}

function setVideoHint(text) {
  const el = document.getElementById("videoHint");
  if (el) el.textContent = text || "—";
}

function stopVideoLoopTimer() {
  try {
    if (state.video.loopTimer) clearInterval(state.video.loopTimer);
  } catch (_) {}
  state.video.loopTimer = 0;
}

function startVideoLoopTimer(ff, baseSrc) {
  stopVideoLoopTimer();
  if (!(ff === "FF_B" || ff === "FF_C" || ff === "FF_D")) return;

  // reload every SEG_SECONDS to "loop segment"
  state.video.loopTimer = setInterval(() => {
    const frame = document.getElementById("teamVideoFrame");
    if (!frame) return;
    // cache-buster forces restart at "start="
    frame.src = `${baseSrc}${baseSrc.includes("?") ? "&" : "?"}cb=${Date.now()}`;
  }, VIDEO.SEG_SECONDS * 1000);
}

function updateTeamVideo(force = false) {
  const { frame } = ensureTeamVideoSlot();
  if (!frame) return;

  const ff = state.selectedFF;

  let src = "about:blank";
  let hint = "";

  if (ff === "FF_A") {
    src = VIDEO.FF_A.url;
    hint = "FF_A live feed (mtxmedia).";
  } else if (ff === "FF_B" || ff === "FF_C" || ff === "FF_D") {
    const start = VIDEO.START_BY_FF[ff] ?? 0;
    src = ytEmbedUrlNocookie(VIDEO.YT_VIDEO_ID, start);
    hint = `${ff} demo feed (loops every ${VIDEO.SEG_SECONDS}s from ${start}s).`;
  } else {
    hint = "No video source for this unit.";
  }

  if (force || state.video.lastFf !== ff || state.video.lastSrc !== src) {
    stopVideoLoopTimer();
    frame.src = src;
    state.video.lastFf = ff;
    state.video.lastSrc = src;
    setVideoHint(hint);
    startVideoLoopTimer(ff, src);
  }
}

/* ---------------- Team page ---------------- */

async function updateTeamPage() {
  const ff = state.selectedFF;
  const m = state.latestByFf[ff] || null;

  const nameSub = document.getElementById("ffNameSub");
  if (nameSub) nameSub.textContent = `${FF_NAMES[ff] || ff} • ${CONFIG.TEAM}`;

  const hr = toNum(m?.hrBpm);
  const stress = toNum(m?.stressIndex);
  const fatigue = toNum(m?.fatigueIndex);
  const risk = toNum(m?.riskScore);

  const setText = (id, txt) => {
    const el = document.getElementById(id);
    if (el) el.textContent = txt;
  };

  setText("idxHr", isNum(hr) ? `${fmt(hr, 0)} bpm` : "—");
  setText("idxStress", isNum(stress) ? fmt(stress, 2) : "—");
  setText("idxFatigue", isNum(fatigue) ? fmt(fatigue, 2) : "—");
  setText("idxRisk", isNum(risk) ? fmt(risk, 2) : "—");

  setText("envTemp", isNum(m?.tempC) ? `${fmt(m.tempC, 1)}°C` : "—");
  setText("envHum", isNum(m?.humidityPct) ? `${fmt(m.humidityPct, 0)}%` : "—");
  setText("envCo", isNum(m?.coPpm) ? fmt(m.coPpm, 1) : "—");
  setText("envMq2", isNum(m?.mq2Raw) ? fmt(m.mq2Raw, 0) : "—");

  const leaderPos = state.lastPos["FF_A"];
  const myPos = state.lastPos[ff];
  const distEl = document.getElementById("distVal");
  if (
    distEl &&
    leaderPos &&
    myPos &&
    isNum(leaderPos.lat) &&
    isNum(leaderPos.lon) &&
    isNum(myPos.lat) &&
    isNum(myPos.lon)
  ) {
    const d = distanceMeters([leaderPos.lat, leaderPos.lon], [myPos.lat, myPos.lon]);
    distEl.textContent = d >= 1000 ? `${fmt(d / 1000, 2)} km` : `${fmt(d, 0)} m`;
  } else if (distEl) distEl.textContent = "—";

  const hint = document.getElementById("metricsHint");
  if (hint) hint.textContent = m?.observedAt ? `Updated: ${m.observedAt}` : "—";

  const sev = m ? computeSeverity(m) : "warn";
  const b = document.getElementById("ffStatusBadge");
  const big = document.getElementById("ffStatusBig");

  if (big) {
    big.classList.remove("ok", "warn", "danger");
    big.classList.add(sev);
  }

  if (sev === "danger") {
    setBadge(b, "danger", "DANGER");
    if (big)
      big.innerHTML = `<div class="ff-status-title">Status: In Danger</div><div class="ff-status-desc">Immediate action recommended.</div>`;
  } else if (sev === "warn") {
    setBadge(b, "warn", "WARNING");
    if (big)
      big.innerHTML = `<div class="ff-status-title">Status: Warning</div><div class="ff-status-desc">Monitor closely.</div>`;
  } else {
    setBadge(b, "ok", "SAFE");
    if (big)
      big.innerHTML = `<div class="ff-status-title">Status: Safe</div><div class="ff-status-desc">No immediate risk detected.</div>`;
  }

  updateTeamVideo(false);
  updateGrafanaFrames(false);
}

/* ---------------- Risk page ---------------- */

function sevFromScore01(x01) {
  if (!isNum(x01)) return "warn";
  if (x01 >= 0.66) return "danger";
  if (x01 >= 0.33) return "warn";
  return "ok";
}

function updateRiskPage() {
  const tbody = document.getElementById("riskTbody");
  const pill = document.getElementById("riskSelectedPill");
  if (pill) pill.textContent = `Selected: ${state.selectedFF}`;

  const rows = [];
  for (const ffId of FF_FIXED) {
    const m = state.latestByFf[ffId] || {};
    rows.push({
      ffId,
      riskScore: toNum(m.riskScore),
      heatRisk: toNum(m.heatRisk),
      gasRisk: toNum(m.gasRisk),
      separationRisk: toNum(m.separationRisk),
      stressIndex: toNum(m.stressIndex),
      fatigueIndex: toNum(m.fatigueIndex),
    });
  }

  if (tbody) {
    tbody.innerHTML = rows
      .map((r) => {
        const sev = sevFromScore01(r.riskScore);
        const active = r.ffId === state.selectedFF ? ' style="outline:2px solid rgba(37,99,235,.25)"' : "";
        return `
        <tr class="risk-row" data-ff="${escapeHtml(r.ffId)}"${active}>
          <td><b>${escapeHtml(r.ffId)}</b></td>
          <td><span class="pill ${sev}">${isNum(r.riskScore) ? fmt(r.riskScore, 2) : "—"}</span></td>
          <td>${isNum(r.heatRisk) ? fmt(r.heatRisk, 2) : "—"}</td>
          <td>${isNum(r.gasRisk) ? fmt(r.gasRisk, 2) : "—"}</td>
          <td>${isNum(r.separationRisk) ? fmt(r.separationRisk, 2) : "—"}</td>
          <td>${isNum(r.stressIndex) ? fmt(r.stressIndex, 2) : "—"}</td>
          <td>${isNum(r.fatigueIndex) ? fmt(r.fatigueIndex, 2) : "—"}</td>
        </tr>
      `;
      })
      .join("");

    tbody.querySelectorAll(".risk-row").forEach((tr) => {
      tr.addEventListener("click", () => {
        const ff = tr.dataset.ff;
        if (!ff) return;

        state.selectedFF = ff;

        document.querySelectorAll(".subtab").forEach((b) =>
          b.classList.toggle("active", b.dataset.ff === state.selectedFF)
        );

        updateGrafanaFrames(true);
        updateTeamVideo(true);

        updateRiskPage();
        if (state.page === "team") updateTeamPage();
      });
    });
  }

  const scores = rows.map((r) => r.riskScore).filter(isNum);
  const teamAvg = scores.length ? scores.reduce((a, b) => a + b, 0) / scores.length : null;
  const worst = rows.reduce(
    (best, r) => (isNum(r.riskScore) && (best == null || r.riskScore > best.riskScore) ? r : best),
    null
  );

  const teamRiskVal = document.getElementById("teamRiskVal");
  if (teamRiskVal) teamRiskVal.textContent = isNum(teamAvg) ? fmt(teamAvg, 2) : "—";

  const worstUnitVal = document.getElementById("worstUnitVal");
  if (worstUnitVal)
    worstUnitVal.textContent = worst
      ? `${worst.ffId} (${isNum(worst.riskScore) ? fmt(worst.riskScore, 2) : "—"})`
      : "—";

  const badge = document.getElementById("teamRiskBadge");
  const sev = sevFromScore01(teamAvg);
  setBadge(badge, sev, sev === "danger" ? "HIGH" : sev === "warn" ? "MEDIUM" : "LOW");
}

function initRiskActions() {
  document.getElementById("btnEvacuateTeam")?.addEventListener("click", async () => {
    try {
      const res = await apiAction({
        teamId: CONFIG.TEAM,
        ffId: "ALL",
        action: "EVACUATE_TEAM",
        note: "Operator initiated evacuation",
      });
      pushActionLog({
        type: "action",
        action: "EVACUATE_TEAM",
        teamId: CONFIG.TEAM,
        ffId: "ALL",
        observedAt: new Date().toISOString(),
        id: res?.id || "",
      });
      flashToast("Evacuation command sent");
      safeCall(updateAlertsPage);
    } catch (e) {
      console.warn(e);
      flashToast("Failed to send evacuation command");
    }
  });

  document.getElementById("btnMedicalSelected")?.addEventListener("click", async () => {
    const ff = state.selectedFF || "FF_A";
    try {
      const res = await apiAction({
        teamId: CONFIG.TEAM,
        ffId: ff,
        action: "MEDICAL_ATTENTION",
        note: "Operator requested medical attention",
      });
      pushActionLog({
        type: "action",
        action: "MEDICAL_ATTENTION",
        teamId: CONFIG.TEAM,
        ffId: ff,
        observedAt: new Date().toISOString(),
        id: res?.id || "",
      });
      flashToast(`Medical request sent for ${ff}`);
      safeCall(updateAlertsPage);
    } catch (e) {
      console.warn(e);
      flashToast("Failed to send medical request");
    }
  });
}

/* ---------------- Alerts page ---------------- */

function formatWhen(iso) {
  try {
    return new Date(iso).toLocaleString();
  } catch {
    return iso || "—";
  }
}

function normalizeAlertItem(a) {
  const sev = String(a?.severity || a?.sev || "info").toUpperCase();
  let sevClass = "ok";
  if (sev.includes("HIGH") || sev.includes("CRIT") || sev.includes("DANGER") || sev.includes("RED"))
    sevClass = "danger";
  else if (sev.includes("MED") || sev.includes("WARN") || sev.includes("YELL")) sevClass = "warn";

  return {
    kind: "alert",
    observedAt: a?.observedAt || a?._time || "",
    ffId: a?.ffId || "",
    title: a?.title || a?.category || "Alert",
    detail: a?.detail || a?.message || "",
    sevClass,
    sevText: sev,
  };
}

function normalizeActionItem(a) {
  return {
    kind: "action",
    observedAt: a?.observedAt || "",
    ffId: a?.ffId || "ALL",
    title: a?.action || "Action",
    detail: a?.note || "",
    sevClass: a?.action?.includes("EVACUATE") ? "danger" : "warn",
    sevText: "COMMAND",
  };
}

async function updateAlertsPage() {
  const badge = document.getElementById("alertsBadge");
  const log = document.getElementById("alertsLog");
  if (!log) return;

  let items = [];

  try {
    const a = await apiAlerts(CONFIG.ACTIONS_MINUTES);
    const arr = Array.isArray(a?.alerts) ? a.alerts : [];
    items = items.concat(arr.map(normalizeAlertItem));
    state.alertsCache = arr;
  } catch (e) {
    console.warn(e);
  }

  items = items.concat((state.actionLog || []).map(normalizeActionItem));
  items.sort((x, y) => String(y.observedAt || "").localeCompare(String(x.observedAt || "")));

  if (!items.length) {
    log.innerHTML = `<div class="hint">No alerts/actions yet.</div>`;
    setBadge(badge, "ok", "NOMINAL");
    return;
  }

  const worst = items.find((i) => i.sevClass === "danger")
    ? "danger"
    : items.find((i) => i.sevClass === "warn")
    ? "warn"
    : "ok";
  setBadge(badge, worst, worst === "danger" ? "DANGER" : worst === "warn" ? "WARNING" : "NOMINAL");

  log.innerHTML = items
    .slice(0, 200)
    .map(
      (i) => `
    <div class="log-item">
      <div class="log-top">
        <div>
          <div class="log-title">${escapeHtml(i.title)} ${
        i.ffId ? `<span class="pill ${i.sevClass}">${escapeHtml(i.ffId)}</span>` : ""
      }</div>
          <div class="log-meta">${escapeHtml(formatWhen(i.observedAt))} • ${i.kind.toUpperCase()}</div>
        </div>
        <span class="badge ${i.sevClass}">${escapeHtml(i.sevText)}</span>
      </div>
      ${i.detail ? `<div class="log-detail">${escapeHtml(i.detail)}</div>` : ""}
    </div>
  `
    )
    .join("");
}

/* ---------------- Medical page (unchanged from your ref) ---------------- */

const ECG = { ws: null, connected: false, fs: 130.0, lastTs: null, lastCount: null, samples: [], maxSec: 12.0, raf: 0 };

function s24leToInt(b0, b1, b2) { let v = (b0) | (b1 << 8) | (b2 << 16); if (v & 0x00800000) v |= 0xff000000; return v | 0; }

function parseEcg1Bundle(u8) {
  if (!u8 || u8.length < 9) return [];
  const head = String.fromCharCode(u8[0],u8[1],u8[2],u8[3]);
  if (head !== "ECG1") return [];
  const count = u8[8] || 0;
  let idx = 9;
  const pkts = [];
  for (let i=0;i<count;i++){
    if (idx + 2 > u8.length) break;
    const L = u8[idx] | (u8[idx+1] << 8);
    idx += 2;
    if (idx + L > u8.length) break;
    pkts.push(u8.slice(idx, idx+L));
    idx += L;
  }
  return pkts;
}

function parsePolarPmdEcg(pkt) {
  if (!pkt || pkt.length < 10) return null;
  if (pkt[0] !== 0x00) return null;
  let ts = 0n;
  for (let i=0;i<8;i++) ts |= (BigInt(pkt[1+i]) << (8n*BigInt(i)));
  const data = pkt.slice(10);
  const n = Math.floor(data.length / 3);
  if (n <= 0) return { ts: Number(ts), samples: [] };
  const out = new Array(n);
  for (let i=0;i<n;i++){
    const j = i*3;
    out[i] = s24leToInt(data[j], data[j+1], data[j+2]);
  }
  return { ts: Number(ts), samples: out };
}

function ecgSetStatus(txt) { const el = document.getElementById("ecg-status"); if (el) el.textContent = txt; }
function ecgEnsureBuffer() {
  const maxLen = Math.max(2000, Math.floor(ECG.fs * ECG.maxSec * 1.25));
  if (ECG.samples.length > maxLen) ECG.samples = ECG.samples.slice(ECG.samples.length - maxLen);
}
function ecgAppendSamples(samples, ts) {
  if (!samples || !samples.length) return;
  if (ECG.lastTs != null && ts != null && ts > ECG.lastTs && ECG.lastCount != null) {
    const dtNs = ts - ECG.lastTs;
    const dtSec = dtNs / 1e9;
    if (dtSec > 0.001 && dtSec < 2.0) {
      const fsNew = ECG.lastCount / dtSec;
      if (fsNew > 50 && fsNew < 300) ECG.fs = 0.9 * ECG.fs + 0.1 * fsNew;
    }
  }
  ECG.lastTs = ts;
  ECG.lastCount = samples.length;
  for (const v of samples) ECG.samples.push(Number(v));
  ecgEnsureBuffer();
}
function drawEcg() {
  const canvas = document.getElementById("ecg-canvas");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  if (!ctx) return;

  const w = canvas.width, h = canvas.height;
  ctx.clearRect(0,0,w,h);

  ctx.globalAlpha = 0.08;
  for (let x=0; x<w; x+=50) { ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,h); ctx.stroke(); }
  for (let y=0; y<h; y+=40) { ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(w,y); ctx.stroke(); }
  ctx.globalAlpha = 1.0;

  const fs = ECG.fs || 130;
  const nShow = Math.min(ECG.samples.length, Math.floor(fs * ECG.maxSec));
  if (nShow < 5) { ECG.raf = requestAnimationFrame(drawEcg); return; }

  const seg = ECG.samples.slice(ECG.samples.length - nShow);
  const sorted = [...seg].sort((a,b)=>a-b);
  const med = sorted[Math.floor(sorted.length/2)];
  const absDev = seg.map(v=>Math.abs(v-med)).sort((a,b)=>a-b);
  const mad = absDev[Math.floor(absDev.length/2)] || 1;

  const yScale = (h*0.42) / (mad*12);
  const xScale = w / (nShow-1);

  ctx.beginPath();
  for (let i=0;i<nShow;i++){
    const x = i * xScale;
    const y = h/2 - (seg[i]-med) * yScale;
    if (i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
  }
  ctx.stroke();
  ECG.raf = requestAnimationFrame(drawEcg);
}
function ecgDisconnect() { try { if (ECG.ws) ECG.ws.close(); } catch (_) {} ECG.ws=null; ECG.connected=false; ecgSetStatus("Disconnected"); }
function ecgConnect(team, ff) {
  ecgDisconnect();
  ECG.samples=[]; ECG.lastTs=null; ECG.lastCount=null; ECG.fs=130;

  const url = `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws/ecg?team=${encodeURIComponent(team)}&ff=${encodeURIComponent(ff)}`;
  ecgSetStatus(`Connecting… (${team}/${ff})`);

  let ws;
  try { ws = new WebSocket(url); } catch (e) { console.warn(e); ecgSetStatus("WebSocket init failed"); return; }
  ws.binaryType = "arraybuffer";

  ws.onopen = () => { ECG.ws=ws; ECG.connected=true; ecgSetStatus("Connected"); if (!ECG.raf) ECG.raf = requestAnimationFrame(drawEcg); };
  ws.onmessage = (ev) => {
    try {
      if (!ev.data || (ev.data instanceof ArrayBuffer && ev.data.byteLength === 0)) return;
      const u8 = new Uint8Array(ev.data);
      const pkts = parseEcg1Bundle(u8);
      if (!pkts.length) return;
      for (const p of pkts) {
        const parsed = parsePolarPmdEcg(p);
        if (!parsed) continue;
        ecgAppendSamples(parsed.samples, parsed.ts);
      }
    } catch (e) { console.warn(e); }
  };
  ws.onclose = () => { ECG.connected=false; ecgSetStatus("Disconnected (WS closed)"); };
  ws.onerror = () => { ECG.connected=false; ecgSetStatus("WebSocket error"); };
  ECG.ws = ws;
}

function renderBarRow(metricKey, label, value01) {
  const clamp01 = (x) => Math.max(0, Math.min(1, x));
  const levelLabel = (x01) => {
    if (x01 == null || Number.isNaN(x01)) return "—";
    if (x01 < 0.33) return "Low";
    if (x01 < 0.66) return "Moderate";
    return "High";
  };

  const v = value01 == null ? null : clamp01(Number(value01));
  const pct = v == null || Number.isNaN(v) ? 0 : Math.round(v * 100);
  const lvl = levelLabel(v);

  let sev = "ok";
  if (v == null || Number.isNaN(v)) sev = "warn";
  else if (v >= 0.66) sev = "danger";
  else if (v >= 0.33) sev = "warn";

  const valTxt = v == null || Number.isNaN(v) ? "—" : `${pct}% · ${lvl}`;

  return `
    <div class="bar-row ${sev}" data-metric="${escapeHtml(metricKey)}">
      <div class="bar-label">${escapeHtml(label)}</div>
      <div class="bar-track"><div class="bar-fill" style="width:${pct}%;"></div></div>
      <div class="bar-val">${escapeHtml(valTxt)}</div>
    </div>
  `;
}

function updateMedicalSidePanels() {
  const ff = document.getElementById("med-ff")?.value || "FF_A";
  const m = state.latestByFf[ff] || {};

  const mm = document.getElementById("medical-metrics");
  const pm = document.getElementById("polar-metrics");
  const bars = document.getElementById("medical-bars");

  const card = (k, v) =>
    `<div class="metric-card"><div class="m-v">${escapeHtml(v)}</div><div class="m-l">${escapeHtml(
      k
    )}</div></div>`;

  if (mm) {
    mm.innerHTML = [
      card("WS", ECG.connected ? "LIVE" : "OFF"),
      card("ECG fs", isNum(ECG.fs) ? `${fmt(ECG.fs, 1)} Hz` : "—"),
      card("Samples", ECG.samples.length ? String(ECG.samples.length) : "—"),
      card("Window", `${ECG.maxSec.toFixed(0)}s`),
    ].join("");
  }

  if (pm) {
    pm.innerHTML = [
      card("HR", isNum(m.hrBpm) ? `${fmt(m.hrBpm, 0)} bpm` : "—"),
      card("RR", isNum(m.rrMs) ? `${fmt(m.rrMs, 0)} ms` : "—"),
      card("RMSSD", isNum(m.rmssdMs) ? `${fmt(m.rmssdMs, 1)} ms` : "—"),
      card("SDNN", isNum(m.sdnnMs) ? `${fmt(m.sdnnMs, 1)} ms` : "—"),
      card("pNN50", isNum(m.pnn50Pct) ? `${fmt(m.pnn50Pct, 1)} %` : "—"),
      card("Risk", isNum(m.riskScore) ? fmt(m.riskScore, 2) : "—"),
    ].join("");
  }

  if (bars) {
    bars.innerHTML = [
      renderBarRow("stress", "Stress", toNum(m.stressIndex)),
      renderBarRow("fatigue", "Fatigue", toNum(m.fatigueIndex)),
      renderBarRow("heat", "Heat", toNum(m.heatRisk)),
      renderBarRow("gas", "Gas", toNum(m.gasRisk)),
      renderBarRow("separation", "Separation", toNum(m.separationRisk)),
    ].join("");
  }
}

function updateMedicalPage() {
  updateMedicalSidePanels();
}

function initMedical() {
  document.getElementById("btn-ecg-connect")?.addEventListener("click", () => {
    const team = document.getElementById("med-team")?.value || CONFIG.TEAM;
    const ff = document.getElementById("med-ff")?.value || "FF_A";
    ecgConnect(team, ff);
    flashToast(`ECG: ${team}/${ff}`);
  });

  document.getElementById("med-ff")?.addEventListener("change", () => updateMedicalSidePanels());
  document.getElementById("med-team")?.addEventListener("change", () => updateMedicalSidePanels());

  document.getElementById("btn-medical-action")?.addEventListener("click", async () => {
    const team = document.getElementById("med-team")?.value || CONFIG.TEAM;
    const ff = document.getElementById("med-ff")?.value || "FF_A";
    try {
      const res = await apiAction({
        teamId: team,
        ffId: ff,
        action: "MEDICAL_ATTENTION",
        note: "Medical tab request",
      });
      pushActionLog({
        type: "action",
        action: "MEDICAL_ATTENTION",
        teamId: team,
        ffId: ff,
        observedAt: new Date().toISOString(),
        id: res?.id || "",
      });
      flashToast(`Medical request sent for ${ff}`);
      safeCall(updateAlertsPage);
    } catch (e) {
      console.warn(e);
      flashToast("Failed to send medical request");
    }
  });

  if (document.getElementById("ecg-canvas") && !ECG.raf) ECG.raf = requestAnimationFrame(drawEcg);
}

/* ---------------- Actions pull (optional) ---------------- */

async function pollActionsIfDue() {
  const now = Date.now();
  if (now - state.lastActionsPollAt < CONFIG.ACTIONS_POLL_MS) return;

  try {
    const acts = await apiActions(CONFIG.ACTIONS_MINUTES);
    const arr = Array.isArray(acts?.actions) ? acts.actions : Array.isArray(acts) ? acts : [];
    addActionsFromServer(arr);
  } catch (_) {}

  state.lastActionsPollAt = now;
}

/* ---------------- Poll loop ---------------- */

async function poll() {
  if (state.polling) return;
  state.polling = true;

  try {
    await apiHealth();
    setConn(true, "API Mode");

    const latest = await apiLatest();
    const members = Array.isArray(latest.members) ? latest.members : [];

    const next = {};
    for (const raw of members) {
      const id = canonicalId(raw?.ffId);
      if (!id) continue;
      next[id] = raw;
    }

    state.latestByFf = { ...state.latestByFf, ...next };

    // Render fixed A-D first
    for (const ffId of FF_FIXED) {
      const m = state.latestByFf[ffId] || { ffId };
      upsertMarkerAlways(ffId, m);
    }

    // Extra members (if any)
    for (const [ffId, m] of Object.entries(state.latestByFf)) {
      if (FF_FIXED.includes(ffId)) continue;
      upsertMarkerAlways(ffId, m);
    }

    // ✅ incident circle updated after markers
    updateIncidentCircle();

    // save coords occasionally
    saveLastPos();

    centerOnLeader(false);

    updateDashboardTiles();
    updateTeamStatusPanel();

    await pollWeatherIfDue();
    await pollActionsIfDue();

    if (state.page === "team") await updateTeamPage();
    if (state.page === "risk") safeCall(updateRiskPage);
    if (state.page === "alerts") safeCall(updateAlertsPage);
    if (state.page === "medical") safeCall(updateMedicalSidePanels);
  } catch (e) {
    setConn(false, "API Offline");
  } finally {
    state.polling = false;
  }
}

/* ---------------- Init ---------------- */

document.addEventListener("DOMContentLoaded", () => {
  tickClock();
  setInterval(tickClock, 1000);

  loadLastPos(); // ✅ restore last known coords

  initTabs();
  initMap();
  loadActionLog();
  initRiskActions();
  initMedical();

  showPage("map");
  setConn(true, "API starting…");

  document.querySelectorAll(".team-tile").forEach((tile) => {
    tile.addEventListener("click", (ev) => {
      const team = tile.dataset.team || CONFIG.TEAM;

      if (state.page === "map" && team === CONFIG.TEAM && !ev.shiftKey && !ev.altKey && !ev.metaKey) {
        centerOnLeader(true);
        try {
          window._markers?.["FF_A"]?.openPopup();
        } catch (_) {}
        flashToast(`Zoomed to ${team} leader (FF_A)`);
        return;
      }

      state.selectedFF = "FF_A";
      document
        .querySelectorAll(".subtab")
        .forEach((b) => b.classList.toggle("active", b.dataset.ff === "FF_A"));

      updateGrafanaFrames(true);
      updateTeamVideo(true);
      showPage("team");
    });
  });

  // Ensure the video slot exists early (team tab)
  ensureTeamVideoSlot();
  updateTeamVideo(true);
  updateGrafanaFrames(true);

  // force a weather try immediately on load (don’t wait 5 minutes)
  state.lastWeatherAt = 0;

  poll();
  setInterval(poll, CONFIG.POLL_MS);
});
