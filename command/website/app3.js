/* =========================================================
   Command Center Frontend (Always show FF_A..FF_D)
   - Always renders markers using LAST KNOWN coords if current missing
   - Trails: keep last 10 points; only append when new coords are received
   - Grafana HR panels via iframe
   - Weather via /api/weather using leader (FF_A) last known coords
   ========================================================= */

const CONFIG = {
  TEAM: "Team_A",
  POLL_MS: 2000,
  LATEST_MINUTES: 60,
  WEATHER_POLL_MS: 5 * 60 * 1000,
  TRAIL_POINTS: 10,
  TRAIL_MIN_METERS: 2,
};

const GRAFANA = {
  BASE: "http://192.168.2.12:8087",
  ORG_ID: 1,
  DASH_UID: "ad5htqk",
  DASH_SLUG: "command-center-dashboard",
  FROM: "now-5m",
  TO: "now",
  THEME: "light",
  PANEL_BY_FF: { FF_A: 1, FF_B: 2, FF_C: 3, FF_D: 4 }
};

const FF_NAMES = {
  FF_A: "Alex (Leader)",
  FF_B: "Maria",
  FF_C: "Nikos",
  FF_D: "Eleni",
};

const FF_FIXED = ["FF_A","FF_B","FF_C","FF_D"];

const state = {
  page: "map",
  selectedFF: "FF_A",
  latestByFf: {},        // raw metrics by ffId (whatever keys the API provides)
  polling: false,
  mapCenteredOnce: false,
  lastWeatherAt: 0,

  // last known position per FF (persist forever)
  lastPos: {
    FF_A: null,
    FF_B: null,
    FF_C: null,
    FF_D: null,
  }
};

/* ---------------- Helpers ---------------- */
function isNum(x) { return typeof x === "number" && Number.isFinite(x); }
function fmt(x, d = 0) { return isNum(x) ? x.toFixed(d) : "—"; }

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
  if (x === null || x === undefined) return null;
  // trims spaces/newlines etc
  return String(x).trim();
}

/**
 * If your API sometimes sends "Team_A/FF_A" or similar,
 * this makes it still land as FF_A.
 */
function canonicalId(id) {
  const s = normId(id);
  if (!s) return null;
  // take last path segment if present
  const parts = s.split("/").filter(Boolean);
  const last = parts[parts.length - 1] || s;
  return last.trim();
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
      lat = lat2; lon = lon2;
    } else if (Array.isArray(L.coordinates) && L.coordinates.length >= 2) {
      const lonG = toNum(L.coordinates[0]);
      const latG = toNum(L.coordinates[1]);
      if (isNum(latG) && isNum(lonG)) { lat = latG; lon = lonG; }
    }
  }

  if ((!isNum(lat) || !isNum(lon)) && m.position) {
    const P = m.position;
    const lat2 = toNum(P.lat);
    const lon2 = toNum(P.lon ?? P.lng);
    if (isNum(lat2) && isNum(lon2)) {
      lat = lat2; lon = lon2;
    } else if (Array.isArray(P.coordinates) && P.coordinates.length >= 2) {
      const lonG = toNum(P.coordinates[0]);
      const latG = toNum(P.coordinates[1]);
      if (isNum(latG) && isNum(lonG)) { lat = latG; lon = lonG; }
    }
  }

  return { lat, lon };
}

function clamp01(x) { return Math.max(0, Math.min(1, x)); }

function levelLabel(x01) {
  if (x01 == null || Number.isNaN(x01)) return "—";
  if (x01 < 0.33) return "Low";
  if (x01 < 0.66) return "Moderate";
  return "High";
}

function renderBarRow(label, value01, suffix = "") {
  const v = value01 == null ? null : clamp01(Number(value01));
  const pct = v == null || Number.isNaN(v) ? 0 : Math.round(v * 100);
  const lvl = levelLabel(v);
  const valTxt = v == null || Number.isNaN(v) ? "—" : `${pct}%${suffix ? " " + suffix : ""} · ${lvl}`;
  return `
    <div class="bar-row">
      <div class="small"><b>${escapeHtml(label)}</b></div>
      <div class="bar-track"><div class="bar-fill" style="width:${pct}%;"></div></div>
      <div class="small">${valTxt}</div>
    </div>
  `;
}

function setConn(ok, text) {
  const dot = document.querySelector("#connectionStatus .dot") || document.querySelector(".dot");
  const t = document.querySelector("#connectionStatus .conn-text") || document.querySelector(".conn-text");
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
    hour: "2-digit", minute: "2-digit", second: "2-digit",
    day: "2-digit", month: "short", year: "numeric"
  });
}

/* ---------------- Tabs ---------------- */
function showPage(page) {
  state.page = page;
  document.querySelectorAll(".tab-btn").forEach(b => b.classList.toggle("active", b.dataset.page === page));
  document.querySelectorAll(".page").forEach(sec => {
    const active = sec.id === `page-${page}`;
    sec.classList.toggle("active", active);
    sec.style.display = active ? "block" : "none";
  });

  if (page === "map" && window._map) setTimeout(() => window._map.invalidateSize(), 150);
  if (page === "team") updateTeamPage();
    updateEnvSnapshot();
    updateRiskPage();
    // alerts + medical only refresh when their tab is visible, but keep data ready
    updateMedicalSidePanels();
}

function initTabs() {
  document.querySelectorAll(".tab-btn").forEach(btn => btn.addEventListener("click", () => showPage(btn.dataset.page)));

  document.querySelectorAll(".subtab").forEach(btn => {
    btn.addEventListener("click", () => {
      state.selectedFF = btn.dataset.ff;
      document.querySelectorAll(".subtab").forEach(b => b.classList.toggle("active", b.dataset.ff === state.selectedFF));
      updateTeamPage();
    updateEnvSnapshot();
    updateRiskPage();
    // alerts + medical only refresh when their tab is visible, but keep data ready
    updateMedicalSidePanels();
    });
  });
}

/* ---------------- Severity logic ---------------- */
function computeSeverity(m) {
  let sev = "ok";
  if ((isNum(m.hrBpm) && m.hrBpm >= 150) ||
      (isNum(m.tempC) && m.tempC >= 50) ||
      (isNum(m.mq2Raw) && m.mq2Raw >= 2600) ||
      (isNum(m.stressIndex) && m.stressIndex >= 0.75)) sev = "warn";
  if ((isNum(m.hrBpm) && m.hrBpm >= 170) ||
      (isNum(m.tempC) && m.tempC >= 60) ||
      (isNum(m.mq2Raw) && m.mq2Raw >= 3200) ||
      (isNum(m.stressIndex) && m.stressIndex >= 0.90)) sev = "danger";
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
  const toRad = x => x * Math.PI / 180;
  const dLat = toRad(b[0] - a[0]);
  const dLon = toRad(b[1] - a[1]);
  const lat1 = toRad(a[0]);
  const lat2 = toRad(b[0]);
  const s = Math.sin(dLat/2)**2 + Math.cos(lat1)*Math.cos(lat2)*Math.sin(dLon/2)**2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(s)));
}

/* ---------------- Map ---------------- */
function initMap() {
  const el = document.getElementById("mainMap");
  if (!el) return;

  const map = L.map("mainMap").setView([38.2466, 21.7346], 18);
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
    maxZoom: 20,
    attribution: "© OpenStreetMap"
  }).addTo(map);

  window._map = map;
  window._markers = {};     // ffId -> L.circleMarker
  window._tracks = {};      // ffId -> Array<[lat,lon]>
  window._trackLines = {};  // ffId -> L.polyline

  document.getElementById("btnRecenterLeader")?.addEventListener("click", () => centerOnLeader(true));
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
      <div style="font-weight:1100">${FF_NAMES[ffId] || ffId}
        <span style="color:#54657e;font-weight:900">(${ffId})</span>
      </div>
      <div style="margin-top:6px;color:#54657e;font-size:13px;line-height:1.45">
        📍 ${isNum(lat) ? lat.toFixed(6) : "—"}, ${isNum(lon) ? lon.toFixed(6) : "—"}<br/>
        🕒 ${m?.observedAt || "—"}<br/>
        ❤️ HR: <b>${fmt(m?.hrBpm, 0)}</b><br/>
        🌡️ Temp: <b>${fmt(m?.tempC, 1)}°C</b><br/>
        🫁 Smoke: <b>${fmt(m?.mq2Raw, 0)}</b><br/>
        🧠 Stress: <b>${fmt(m?.stressIndex, 2)}</b>
      </div>
    </div>
  `;
}

/**
 * ALWAYS show marker if we have *any* last-known coordinates.
 * If current coords missing, use lastPos.
 * If current coords present, update lastPos and trail.
 */
function upsertMarkerAlways(ffId, m) {
  if (!window._map) return;

  const c = extractCoords(m);
  const hadFresh = isNum(c.lat) && isNum(c.lon);

  // decide which coords to use
  let lat = c.lat, lon = c.lon;

  if (!hadFresh) {
    const last = state.lastPos[ffId];
    if (last && isNum(last.lat) && isNum(last.lon)) {
      lat = last.lat;
      lon = last.lon;
    }
  } else {
    // store last-known forever
    state.lastPos[ffId] = { lat, lon, ts: Date.now() };
    // only trail on fresh coords
    appendTrailPoint(ffId, lat, lon);
  }

  // If still no coords => never had coords yet, can't place on map
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
      opacity: 1
    }).addTo(window._map);

    mk.bindPopup(popup);
    mk.bindTooltip(FF_NAMES[ffId] || ffId, { direction: "top", offset: [0, -10], opacity: 0.9 });
    store[ffId] = mk;
  } else {
    store[ffId].setLatLng([lat, lon]);
    store[ffId].setStyle({ fillColor: color, color: "#ffffff" });
    store[ffId].getPopup().setContent(popup);
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

  // fallback any visible marker
  const any = Object.values(state.lastPos).find(v => v && isNum(v.lat) && isNum(v.lon));
  if (any && (force || !state.mapCenteredOnce)) {
    window._map.setView([any.lat, any.lon], 18);
    state.mapCenteredOnce = true;
  }
}

/* ---------------- API ---------------- */
async function fetchJson(path) {
  const r = await fetch(path, { cache: "no-store" });
  if (!r.ok) throw new Error(`${r.status} ${path}`);
  return await r.json();
}
const apiHealth = () => fetchJson("/api/health");
const apiLatest = () => fetchJson(`/api/latest?team=${encodeURIComponent(CONFIG.TEAM)}&minutes=${CONFIG.LATEST_MINUTES}`);
const apiWeather = (lat, lon) => fetchJson(`/api/weather?lat=${encodeURIComponent(lat)}&lon=${encodeURIComponent(lon)}`);

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
  let worst = "ok", worstFf = null;
  for (const [ffId, m] of Object.entries(state.latestByFf)) {
    const s = computeSeverity(m);
    if (s === "danger") { worst = "danger"; worstFf = ffId; break; }
    if (s === "warn" && worst !== "danger") { worst = "warn"; worstFf = ffId; }
  }

  const badge = document.getElementById("teamStatusBadge");
  const box = document.getElementById("teamStatusBox");

  if (worst === "danger") {
    setBadge(badge, "danger", "DANGER");
    if (box) box.innerHTML = `<div class="status-title">Critical Condition</div><div class="status-desc">Highest risk: ${FF_NAMES[worstFf] || worstFf}</div>`;
  } else if (worst === "warn") {
    setBadge(badge, "warn", "WARNING");
    if (box) box.innerHTML = `<div class="status-title">Warning Detected</div><div class="status-desc">Watch: ${FF_NAMES[worstFf] || worstFf}</div>`;
  } else {
    setBadge(badge, "ok", "NOMINAL");
    if (box) box.innerHTML = `<div class="status-title">All Systems Nominal</div><div class="status-desc">No active critical conditions detected.</div>`;
  }
}

/* ---------------- Weather ---------------- */
function updateWeatherUI(w) {
  if (isNum(w?.tempC)) document.getElementById("wxTemp").textContent = `${fmt(w.tempC, 1)}°C`;
  if (isNum(w?.windMs)) document.getElementById("wxWind").textContent = `${fmt(w.windMs, 1)} m/s`;
  if (isNum(w?.humidityPct)) document.getElementById("wxHum").textContent = `${fmt(w.humidityPct, 0)}%`;

  const risk = (w?.risk || "LOW").toUpperCase();
  const badge = document.getElementById("weatherBadge");
  if (risk === "HIGH" || risk === "EXTREME") setBadge(badge, "danger", risk);
  else if (risk === "MEDIUM") setBadge(badge, "warn", risk);
  else setBadge(badge, "ok", risk);

  const hint = document.getElementById("wxHint");
  if (hint) hint.textContent = `Weather updated from FF_A (leader) last-known location.`;
}

function getLeaderCoords() {
  const p = state.lastPos["FF_A"];
  if (p && isNum(p.lat) && isNum(p.lon)) return { lat: p.lat, lon: p.lon };
  return null;
}

async function pollWeatherIfDue() {
  const now = Date.now();
  if (now - state.lastWeatherAt < CONFIG.WEATHER_POLL_MS) return;

  const leader = getLeaderCoords();
  if (!leader) return;

  try {
    const w = await apiWeather(leader.lat, leader.lon);
    updateWeatherUI(w);
    state.lastWeatherAt = now;
  } catch {
    setBadge(document.getElementById("weatherBadge"), "warn", "N/A");
  }
}

/* ---------------- Grafana embed ---------------- */
function buildGrafanaPanelUrl(ffId) {
  const base = `${GRAFANA.BASE}/d-solo/${encodeURIComponent(GRAFANA.DASH_UID)}/${encodeURIComponent(GRAFANA.DASH_SLUG)}`;
  const params = new URLSearchParams();
  params.set("orgId", String(GRAFANA.ORG_ID));
  params.set("panelId", String(GRAFANA.PANEL_BY_FF?.[ffId] || 1));
  params.set("from", GRAFANA.FROM);
  params.set("to", GRAFANA.TO);
  params.set("theme", GRAFANA.THEME);
  params.set("kiosk", "tv");
  return `${base}?${params.toString()}`;
}

function updateGrafanaHrFrame() {
  const frame = document.getElementById("hrGrafanaFrame");
  if (!frame) return;
  frame.src = buildGrafanaPanelUrl(state.selectedFF);
  setBadge(document.getElementById("hrPill"), "ok", state.selectedFF);
}

/* ---------------- Team page ---------------- */
async function updateTeamPage() {
  const ff = state.selectedFF;
  const m = state.latestByFf[ff] || null;

  const nameSub = document.getElementById("ffNameSub");
  if (nameSub) nameSub.textContent = `${FF_NAMES[ff] || ff} • ${CONFIG.TEAM}`;

  document.getElementById("stressVal").textContent = isNum(m?.stressIndex) ? fmt(m.stressIndex, 2) : "—";
  document.getElementById("tempVal").textContent = isNum(m?.tempC) ? `${fmt(m.tempC, 1)}°C` : "—";
  document.getElementById("smokeVal").textContent = isNum(m?.mq2Raw) ? fmt(m.mq2Raw, 0) : "—";
  document.getElementById("distVal").textContent = "—";
  document.getElementById("metricsHint").textContent = m?.observedAt ? `Updated: ${m.observedAt}` : "—";

  const sev = m ? computeSeverity(m) : "warn";
  const b = document.getElementById("ffStatusBadge");
  const big = document.getElementById("ffStatusBig");

  if (sev === "danger") {
    setBadge(b, "danger", "DANGER");
    if (big) big.innerHTML = `<div class="ff-status-title">Status: In Danger</div><div class="ff-status-desc">Immediate attention recommended.</div>`;
  } else if (sev === "warn") {
    setBadge(b, "warn", "WARNING");
    if (big) big.innerHTML = `<div class="ff-status-title">Status: Caution</div><div class="ff-status-desc">Monitor vital signs and environment.</div>`;
  } else {
    setBadge(b, "ok", "SAFE");
    if (big) big.innerHTML = `<div class="ff-status-title">Status: Safe</div><div class="ff-status-desc">No immediate risk detected.</div>`;
  }

  updateGrafanaHrFrame();
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

    // Normalize IDs and store
    const next = {};
    for (const raw of members) {
      const id = canonicalId(raw?.ffId);
      if (!id) continue;
      next[id] = raw;
    }

    // Keep previous metrics if a member disappears this tick
    // (so they still show on UI and map keeps lastPos)
    state.latestByFf = { ...state.latestByFf, ...next };

    // ALWAYS attempt to render fixed A-D markers
    for (const ffId of FF_FIXED) {
      const m = state.latestByFf[ffId] || { ffId };
      upsertMarkerAlways(ffId, m);
    }

    // And render any extra members too
    for (const [ffId, m] of Object.entries(state.latestByFf)) {
      if (FF_FIXED.includes(ffId)) continue;
      upsertMarkerAlways(ffId, m);
    }

    centerOnLeader(false);
    updateDashboardTiles();
    updateTeamStatusPanel();
    await pollWeatherIfDue();

    if (state.page === "team") await updateTeamPage();
    updateEnvSnapshot();
    updateRiskPage();
    // alerts + medical only refresh when their tab is visible, but keep data ready
    updateMedicalSidePanels();

  } catch {
    setConn(false, "API Offline");
  } finally {
    state.polling = false;
  }
}

/* ---------------- Init ---------------- */
document.addEventListener("DOMContentLoaded", () => {
  tickClock();
  setInterval(tickClock, 1000);

  initTabs();
  initMap();

  showPage("map");
  setConn(true, "API starting…");

  document.querySelectorAll(".team-tile").forEach(tile => {
    tile.addEventListener("click", () => {
      state.selectedFF = "FF_A";
      document.querySelectorAll(".subtab").forEach(b => b.classList.toggle("active", b.dataset.ff === "FF_A"));
      showPage("team");
    });
  });

  poll();
  setInterval(poll, CONFIG.POLL_MS);
});let map;

function zoomToLeader(teamId = CONFIG.TEAM) {
  const leaderId = "FF_A";
  const key = `${teamId}/${leaderId}`;
  const m = state.latestByFf[key];
  if (m && Number.isFinite(m.lat) && Number.isFinite(m.lon) && map) {
    map.setView([m.lat, m.lon], 18, { animate: true });
    flashToast(`Zoomed to ${teamId} leader (${leaderId})`);
  } else {
    flashToast("Leader location not available yet");
  }
}
