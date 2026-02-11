/* =========================================================
   Command Center Frontend (API-only)
   - Expects nginx to proxy:
       /api/*  -->  FastAPI on host:7070
   - So browser calls SAME-ORIGIN endpoints like:
       GET /api/health
       GET /api/latest?team=Team_A&minutes=60
       GET /api/hr?team=Team_A&ff=FF_A&minutes=30
   ========================================================= */

const CONFIG = {
  TEAM: "Team_A",
  POLL_MS: 2000,
  LATEST_MINUTES: 60,
  HR_MINUTES: 30,
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
  hrChart: null,
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
    // safety: force visibility even if CSS differs
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
  // Basic operator-friendly thresholds
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
  // your CSS defines: .ff-marker-wrap, .ff-ok, .ff-warn, .ff-danger
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

  // fallback: center on first available unit
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
const apiHr = (ff) => fetchJson(`/api/hr?team=${encodeURIComponent(CONFIG.TEAM)}&ff=${encodeURIComponent(ff)}&minutes=${CONFIG.HR_MINUTES}`);

/* ---------------- Dashboard UI ---------------- */
function updateDashboardTiles() {
  // Update “MQTT Connecting…” label into API mode
  setConn(true, "API Mode");

  // Weather demo placeholders
  document.getElementById("wxTemp").textContent = "22°C";
  document.getElementById("wxWind").textContent = "4 m/s";
  document.getElementById("wxHum").textContent = "62%";
  setBadge(document.getElementById("weatherBadge"), "ok", "LOW");

  const have = Object.keys(state.latestByFf).length;
  const subA = document.getElementById("tileSubA");
  if (subA) subA.textContent = `${have}/4 updating`;

  const meta = document.getElementById("teamsMeta");
  if (meta) meta.textContent = `Selected: ${CONFIG.TEAM}`;
}

function updateTeamStatusPanel() {
  // Worst severity among members
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

/* ---------------- Team page ---------------- */
function ensureHrChart() {
  const canvas = document.getElementById("hrChart");
  if (!canvas || state.hrChart) return;

  state.hrChart = new Chart(canvas, {
    type: "line",
    data: { labels: [], datasets: [{ data: [], tension: 0.25, pointRadius: 0, borderWidth: 2 }] },
    options: {
      responsive: true,
      maintainAspectRatio: true,
      animation: false,
      scales: {
        x: { ticks: { maxTicksLimit: 6 } },
        y: { suggestedMin: 50, suggestedMax: 180 }
      },
      plugins: { legend: { display: false } }
    }
  });
}

async function updateTeamPage() {
  ensureHrChart();

  const ff = state.selectedFF;
  const m = state.latestByFf[ff] || null;

  // Header label
  const nameSub = document.getElementById("ffNameSub");
  if (nameSub) nameSub.textContent = `${FF_NAMES[ff] || ff} • ${CONFIG.TEAM}`;

  // Metric boxes
  document.getElementById("stressVal").textContent = isNum(m?.stressIndex) ? fmt(m.stressIndex, 2) : "—";
  document.getElementById("tempVal").textContent = isNum(m?.tempC) ? `${fmt(m.tempC, 1)}°C` : "—";
  document.getElementById("smokeVal").textContent = isNum(m?.mq2Raw) ? fmt(m.mq2Raw, 0) : "—";
  document.getElementById("distVal").textContent = "—";
  document.getElementById("metricsHint").textContent = m?.observedAt ? `Updated: ${m.observedAt}` : "—";

  // Status badge + big box
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

  // HR time series
  try {
    const hr = await apiHr(ff);
    const pts = Array.isArray(hr.points) ? hr.points : [];

    if (state.hrChart) {
      state.hrChart.data.labels = pts.map(p => {
        const d = new Date(p.t);
        return `${String(d.getMinutes()).padStart(2, "0")}:${String(d.getSeconds()).padStart(2, "0")}`;
      });
      state.hrChart.data.datasets[0].data = pts.map(p => p.v);
      state.hrChart.update("none");
    }

    setBadge(document.getElementById("hrPill"), pts.length ? "ok" : "warn", pts.length ? "OK" : "NO DATA");
  } catch {
    setBadge(document.getElementById("hrPill"), "warn", "NO DATA");
  }
}

/* ---------------- Poll loop ---------------- */
async function poll() {
  if (state.polling) return;
  state.polling = true;

  try {
    await apiHealth(); // throws if nginx proxy or API unreachable
    setConn(true, "API Mode");

    const latest = await apiLatest();
    const members = Array.isArray(latest.members) ? latest.members : [];

    const next = {};
    for (const m of members) if (m.ffId) next[m.ffId] = m;
    state.latestByFf = next;

    // Map markers for anyone who has lat/lon
    for (const [ffId, m] of Object.entries(state.latestByFf)) {
      if (isNum(m.lat) && isNum(m.lon)) upsertMarker(ffId, m);
    }

    centerOnLeader(false);
    updateDashboardTiles();
    updateTeamStatusPanel();

    if (state.page === "team") await updateTeamPage();

  } catch (e) {
    setConn(false, "API Offline");
  } finally {
    state.polling = false;
  }
}

/* ---------------- Init ---------------- */
document.addEventListener("DOMContentLoaded", () => {
  // Clock
  tickClock();
  setInterval(tickClock, 1000);

  // Tabs and map
  initTabs();
  initMap();

  // Force start on Map page
  showPage("map");

  // Overwrite "MQTT Connecting…" immediately
  setConn(true, "API starting…");

  // Team tiles click -> go Team page (demo focuses Team A)
  document.querySelectorAll(".team-tile").forEach(tile => {
    tile.addEventListener("click", () => {
      state.selectedFF = "FF_A";
      document.querySelectorAll(".subtab").forEach(b => b.classList.toggle("active", b.dataset.ff === "FF_A"));
      showPage("team");
    });
  });

  // first poll + interval
  poll();
  setInterval(poll, CONFIG.POLL_MS);
});
