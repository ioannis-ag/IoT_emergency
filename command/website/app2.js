/* =========================================================
   Command Center Frontend
   - Uses /api/latest for live metrics + location
   - Embeds Grafana HR panels via iframe (panelId switches per FF)
   - Pulls weather via /api/weather using FF_A leader location every ~5 min
   ========================================================= */

const CONFIG = {
  TEAM: "Team_A",
  POLL_MS: 2000,
  LATEST_MINUTES: 60,
  WEATHER_POLL_MS: 5 * 60 * 1000,
};

const GRAFANA = {
  BASE: "http://192.168.2.12:8087",
  ORG_ID: 1,
  DASH_UID: "ad5htqk",
  DASH_SLUG: "command-center-dashboard",
  FROM: "now-5m",
  TO: "now",
  THEME: "light",

  // ✅ SET THESE TO YOUR REAL PANEL IDs (from Grafana panel share links)
  PANEL_BY_FF: {
    FF_A: 1, // panelId for HR_FF_A
    FF_B: 2, // panelId for HR_FF_B
    FF_C: 3, // panelId for HR_FF_C
    FF_D: 4, // panelId for HR_FF_D
  }
};

const FF_NAMES = {
  FF_A: "Alex (Leader)",
  FF_B: "Maria",
  FF_C: "Nikos",
  FF_D: "Eleni",
};

const state = {
  page: "map",
  selectedFF: "FF_A",
  latestByFf: {},
  polling: false,
  mapCenteredOnce: false,
  lastWeatherAt: 0,
};

/* ---------------- Helpers ---------------- */
function isNum(x) { return typeof x === "number" && Number.isFinite(x); }
function fmt(x, d = 0) { return isNum(x) ? x.toFixed(d) : "—"; }

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

  document.querySelectorAll(".tab-btn").forEach(b => {
    b.classList.toggle("active", b.dataset.page === page);
  });

  document.querySelectorAll(".page").forEach(sec => {
    const active = sec.id === `page-${page}`;
    sec.classList.toggle("active", active);
    sec.style.display = active ? "block" : "none";
  });

  if (page === "map" && window._map) setTimeout(() => window._map.invalidateSize(), 150);
  if (page === "team") updateTeamPage();
}

function initTabs() {
  document.querySelectorAll(".tab-btn").forEach(btn => {
    btn.addEventListener("click", () => showPage(btn.dataset.page));
  });

  document.querySelectorAll(".subtab").forEach(btn => {
    btn.addEventListener("click", () => {
      state.selectedFF = btn.dataset.ff;
      document.querySelectorAll(".subtab").forEach(b => b.classList.toggle("active", b.dataset.ff === state.selectedFF));
      updateTeamPage();
    });
  });
}

/* ---------------- Severity logic ---------------- */
function computeSeverity(m) {
  let sev = "ok";
  if ((isNum(m.hrBpm) && m.hrBpm >= 150) ||
      (isNum(m.tempC) && m.tempC >= 50) ||
      (isNum(m.mq2Raw) && m.mq2Raw >= 2600) ||
      (isNum(m.stressIndex) && m.stressIndex >= 0.75)) {
    sev = "warn";
  }
  if ((isNum(m.hrBpm) && m.hrBpm >= 170) ||
      (isNum(m.tempC) && m.tempC >= 60) ||
      (isNum(m.mq2Raw) && m.mq2Raw >= 3200) ||
      (isNum(m.stressIndex) && m.stressIndex >= 0.90)) {
    sev = "danger";
  }
  return sev;
}

/* ---------------- Map ---------------- */
function markerIcon(sev = "ok") {
  const cls = sev === "danger" ? "ff-danger" : (sev === "warn" ? "ff-warn" : "ff-ok");
  return L.divIcon({
    className: "",
    html: `<div class="ff-marker-wrap ${cls}"></div>`,
    iconSize: [18, 18],
    iconAnchor: [9, 9]
  });
}

function initMap() {
  const el = document.getElementById("mainMap");
  if (!el) return;

  const map = L.map("mainMap").setView([38.2466, 21.7346], 18);
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
    maxZoom: 20,
    attribution: "© OpenStreetMap"
  }).addTo(map);

  window._map = map;
  window._markers = {};

  document.getElementById("btnRecenterLeader")?.addEventListener("click", () => centerOnLeader(true));
}

function upsertMarker(ffId, m) {
  if (!window._map || !m) return;
  if (!isNum(m.lat) || !isNum(m.lon)) return;

  const store = window._markers;
  const sev = computeSeverity(m);

  const popup = `
    <div style="min-width:220px">
      <div style="font-weight:1100">${FF_NAMES[ffId] || ffId}
        <span style="color:#54657e;font-weight:900">(${ffId})</span>
      </div>
      <div style="margin-top:6px;color:#54657e;font-size:13px;line-height:1.45">
        📍 ${m.lat.toFixed(6)}, ${m.lon.toFixed(6)}<br/>
        🕒 ${m.observedAt || "—"}<br/>
        ❤️ HR: <b>${fmt(m.hrBpm, 0)}</b><br/>
        🌡️ Temp: <b>${fmt(m.tempC, 1)}°C</b><br/>
        🫁 Smoke: <b>${fmt(m.mq2Raw, 0)}</b><br/>
        🧠 Stress: <b>${fmt(m.stressIndex, 2)}</b>
      </div>
    </div>
  `;

  if (!store[ffId]) {
    const mk = L.marker([m.lat, m.lon], { icon: markerIcon(sev) }).addTo(window._map);
    mk.bindPopup(popup);
    mk.bindTooltip(FF_NAMES[ffId] || ffId, { direction: "top", offset: [0, -10], opacity: 0.9 });
    store[ffId] = mk;
  } else {
    store[ffId].setLatLng([m.lat, m.lon]);
    store[ffId].setIcon(markerIcon(sev));
    store[ffId].getPopup().setContent(popup);
  }
}

function centerOnLeader(force = false) {
  if (!window._map) return;

  const leader = state.latestByFf["FF_A"];
  if (leader && isNum(leader.lat) && isNum(leader.lon)) {
    if (force || !state.mapCenteredOnce) {
      window._map.setView([leader.lat, leader.lon], 18);
      state.mapCenteredOnce = true;
    }
    return;
  }

  const any = Object.values(state.latestByFf).find(m => isNum(m.lat) && isNum(m.lon));
  if (any && (force || !state.mapCenteredOnce)) {
    window._map.setView([any.lat, any.lon], 18);
    state.mapCenteredOnce = true;
  }
}

/* ---------------- API (same origin) ---------------- */
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
  let worst = "ok";
  let worstFf = null;

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
  if (hint) hint.textContent = `Weather updated from FF_A location.`;
}

async function pollWeatherIfDue() {
  const now = Date.now();
  if (now - state.lastWeatherAt < CONFIG.WEATHER_POLL_MS) return;

  const leader = state.latestByFf["FF_A"];
  if (!leader || !isNum(leader.lat) || !isNum(leader.lon)) return;

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

  const panelId = GRAFANA.PANEL_BY_FF?.[ffId] || GRAFANA.PANEL_BY_FF?.FF_A || 1;
  params.set("panelId", String(panelId));

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

  // show which FF is selected in the pill
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

    const next = {};
    for (const m of members) if (m.ffId) next[m.ffId] = m;
    state.latestByFf = next;

    for (const [ffId, m] of Object.entries(state.latestByFf)) {
      if (isNum(m.lat) && isNum(m.lon)) upsertMarker(ffId, m);
    }

    centerOnLeader(false);
    updateDashboardTiles();
    updateTeamStatusPanel();

    await pollWeatherIfDue();

    if (state.page === "team") await updateTeamPage();

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
});
