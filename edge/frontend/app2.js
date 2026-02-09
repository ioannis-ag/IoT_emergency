/* === CONFIGURATION === */
const CONFIG = {
  MQTT_HOST: window.location.hostname || "localhost",
  MQTT_PORT: 9001, // Websocket port
  TEAM_TOPIC: "edge/status/Team_A/#",
  ALERT_TOPIC: "edge/alerts/Team_A/#"
};

/* === STATE === */
const state = {
  members: {},         // ffId -> latest summary
  lastPos: {},         // ffId -> {lat, lon, t}
  alertsByFf: {},      // ffId -> last alerts packet
  focusedId: "FF_A",
  toastTimer: null
};

/* === MAP SETUP === */
let map, mapFocus;
const markers = {};
const markersFocus = {};

function initMaps() {
  // Patras, zoomed in, smaller map area is handled by CSS
  map = L.map("mainMap", { zoomControl: true }).setView([38.2466, 21.7346], 19);
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
    maxZoom: 20,
    attribution: "© OpenStreetMap"
  }).addTo(map);

  mapFocus = L.map("focusMap", { zoomControl: true }).setView([38.2466, 21.7346], 19);
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
    maxZoom: 20,
    attribution: "© OpenStreetMap"
  }).addTo(mapFocus);

  document.getElementById("btnRecenter").onclick = () => {
    const a = state.members["FF_A"];
    if (a && isNum(a.lat) && isNum(a.lon)) map.setView([a.lat, a.lon], 19);
  };
}

/* === UTIL === */
function isNum(x) { return typeof x === "number" && Number.isFinite(x); }

function fmt(x, unit = "", d = 0) {
  if (!isNum(x)) return "—";
  return `${x.toFixed(d)}${unit}`;
}

function severityLabel(sev) {
  if (sev === "danger") return "DANGER";
  if (sev === "warn") return "WARN";
  return "OK";
}

function severityRank(sev) {
  return { ok: 0, warn: 1, danger: 2 }[sev] ?? 0;
}

function bearingDeg(lat1, lon1, lat2, lon2) {
  const toRad = (v) => (v * Math.PI) / 180;
  const toDeg = (v) => (v * 180) / Math.PI;

  const φ1 = toRad(lat1), φ2 = toRad(lat2);
  const Δλ = toRad(lon2 - lon1);

  const y = Math.sin(Δλ) * Math.cos(φ2);
  const x = Math.cos(φ1) * Math.sin(φ2) - Math.sin(φ1) * Math.cos(φ2) * Math.cos(Δλ);

  let θ = toDeg(Math.atan2(y, x));
  θ = (θ + 360) % 360;
  return θ;
}

/* === MARKER ICON === */
function getIcon(severity, headingDeg = 0) {
  const sev = severity || "ok";
  const html = `
    <div class="ff-marker-wrap">
      <div class="ff-arrow" style="transform: translateX(-50%) rotate(${headingDeg}deg)"></div>
      <div class="ff-pin ${sev}">
        <i class="fa-solid fa-fire-flame-curved" style="color:white; font-size:14px;"></i>
      </div>
    </div>
  `;

  return L.divIcon({
    className: "ff-marker",
    html,
    iconSize: [34, 34],
    iconAnchor: [17, 17]
  });
}

function popupHtml(m) {
  const dist = isNum(m.distanceToLeaderM) ? `${Math.round(m.distanceToLeaderM)} m` : "—";
  const lastSeen = (m.lastSeenSec === null || m.lastSeenSec === undefined) ? "—" : `${m.lastSeenSec}s ago`;

  return `
    <div class="popup">
      <div class="p-title">${m.name} <span style="color:#6b7280;font-weight:800">(${m.ffId})</span></div>
      <div class="p-row">Status: <b>${severityLabel(m.severity)}</b></div>
      <hr/>
      <div class="p-row">❤️ Pulse: <b>${fmt(m.pulse, " bpm", 0)}</b> <span style="color:#6b7280">(${m.pulseLevel || "—"})</span></div>
      <div class="p-row">🌡️ Temp: <b>${fmt(m.temp, "°C", 1)}</b> <span style="color:#6b7280">(${m.heatLevel || "—"})</span></div>
      <div class="p-row">🫁 Smoke: <b>${fmt(m.gas, "", 0)}</b> <span style="color:#6b7280">(${m.smokeLevel || "—"})</span></div>
      <hr/>
      <div class="p-row">📍 Distance to leader: <b>${dist}</b></div>
      <div class="p-row">🕒 Last update: <b>${lastSeen}</b></div>
    </div>
  `;
}

function updateMarker(mapObj, store, id, data) {
  const { lat, lon, severity } = data;
  if (!isNum(lat) || !isNum(lon)) return;

  // heading from previous position
  let heading = 0;
  const prev = state.lastPos[id];
  if (prev && isNum(prev.lat) && isNum(prev.lon) && (prev.lat !== lat || prev.lon !== lon)) {
    heading = bearingDeg(prev.lat, prev.lon, lat, lon);
  }
  state.lastPos[id] = { lat, lon, t: Date.now() };

  if (!store[id]) {
    const m = L.marker([lat, lon], { icon: getIcon(severity, heading) }).addTo(mapObj);
    m.bindPopup(popupHtml(data));
    m.bindTooltip(data.name, { direction: "top", offset: [0, -10], opacity: 0.9 });
    store[id] = m;
  } else {
    const m = store[id];
    m.setLatLng([lat, lon]);
    m.setIcon(getIcon(severity, heading));
    m.getPopup().setContent(popupHtml(data));
    m.setTooltipContent(data.name);
  }
}

/* === TEAM LIST === */
function renderTeamList() {
  const list = document.getElementById("teamList");
  const select = document.getElementById("focusSelect");
  const currentSel = select.value || state.focusedId;

  list.innerHTML = "";
  select.innerHTML = "";

  const members = Object.values(state.members)
    .filter(m => m && m.ffId)
    .sort((a, b) => (a.ffId || "").localeCompare(b.ffId || ""));

  members.forEach(m => {
    const pillClass = m.severity || "ok";
    const lastSeen = (m.lastSeenSec === null || m.lastSeenSec === undefined) ? "—" : `${m.lastSeenSec}s`;

    const div = document.createElement("div");
    div.className = "member-card";
    div.innerHTML = `
      <div>
        <div class="member-name">${m.name}</div>
        <div class="member-sub">
          <span>❤️ ${fmt(m.pulse, "", 0)}</span>
          <span>🌡️ ${fmt(m.temp, "", 1)}</span>
          <span>🫁 ${fmt(m.gas, "", 0)}</span>
          <span>⏱ ${lastSeen}</span>
        </div>
      </div>
      <div class="pill ${pillClass}">${severityLabel(m.severity)}</div>
    `;

    div.onclick = () => {
      if (isNum(m.lat) && isNum(m.lon)) {
        map.setView([m.lat, m.lon], 19);
        markers[m.ffId]?.openPopup();
      }
    };

    list.appendChild(div);

    const opt = document.createElement("option");
    opt.value = m.ffId;
    opt.text = `${m.name}`;
    select.appendChild(opt);
  });

  if (currentSel) select.value = currentSel;
}

/* === ALERTS UI (single banner + single toast) === */
function worstOverall() {
  let w = "ok";
  for (const m of Object.values(state.members)) {
    if (!m) continue;
    if (severityRank(m.worst || "ok") > severityRank(w)) w = (m.worst || "ok");
  }
  return w;
}

function updateAlertBox() {
  const box = document.getElementById("alertBox");
  const content = document.getElementById("alertContent");
  const badge = document.getElementById("alertBadge");

  // Determine who has active alerts (from alertsByFf packets)
  const active = Object.values(state.alertsByFf)
    .filter(p => p && Array.isArray(p.alerts) && p.alerts.length > 0)
    .sort((a, b) => severityRank(b.worst) - severityRank(a.worst));

  const w = worstOverall();
  badge.classList.remove("warn", "danger");
  if (w === "danger") badge.classList.add("danger");
  if (w === "warn") badge.classList.add("warn");
  badge.textContent = (w === "danger") ? "Danger" : (w === "warn") ? "Warning" : "Nominal";

  content.innerHTML = "";

  if (active.length === 0) {
    content.innerHTML = `<div class="alert-empty">All systems nominal. No active threats.</div>`;
    return;
  }

  // Show ONLY top 1 block prominently (no flooding)
  const top = active[0];
  const ffId = top.ffId;
  const name = top.name || ffId;

  const block = document.createElement("div");
  block.className = "alert-block";
  block.innerHTML = `
    <div class="ab-head">
      <div class="ab-title">${name}</div>
      <div class="pill ${top.worst}">${severityLabel(top.worst)}</div>
    </div>
    <div class="ab-meta">Grouped alerts (debounced). Click unit on map for details.</div>
    <div class="alert-grid" id="alertGrid"></div>
  `;
  content.appendChild(block);

  const grid = block.querySelector("#alertGrid");
  top.alerts.slice(0, 4).forEach(a => {
    const item = document.createElement("div");
    item.className = `alert-item ${a.severity}`;
    item.innerHTML = `
      <div class="ai-title">${a.title}</div>
      <div class="ai-detail">${a.detail}</div>
    `;
    grid.appendChild(item);
  });
}

function showToast(sev, title, body) {
  const toast = document.getElementById("toast");
  toast.classList.remove("hidden", "warn", "danger");
  if (sev === "danger") toast.classList.add("danger");
  if (sev === "warn") toast.classList.add("warn");

  toast.innerHTML = `
    <div class="t-title">
      <i class="fa-solid fa-bell"></i>
      <span>${title}</span>
    </div>
    <div class="t-body">${body}</div>
  `;

  if (state.toastTimer) clearTimeout(state.toastTimer);
  state.toastTimer = setTimeout(() => {
    toast.classList.add("hidden");
  }, 5000);
}

/* === FOCUS TAB === */
function updateFocusStats() {
  const select = document.getElementById("focusSelect");
  const id = select.value || state.focusedId;
  state.focusedId = id;

  const m = state.members[id];
  if (!m) return;

  const con = document.getElementById("focusStats");
  const hints = document.getElementById("focusHints");

  const dist = isNum(m.distanceToLeaderM) ? `${Math.round(m.distanceToLeaderM)} m` : "—";
  const lastSeen = (m.lastSeenSec === null || m.lastSeenSec === undefined) ? "—" : `${m.lastSeenSec}s ago`;

  con.innerHTML = `
    <div class="stat">
      <div class="sv">${fmt(m.pulse, "", 0)}</div>
      <div class="sl">Pulse (${m.pulseLevel || "—"})</div>
    </div>
    <div class="stat">
      <div class="sv">${fmt(m.temp, "", 1)}°C</div>
      <div class="sl">Temperature (${m.heatLevel || "—"})</div>
    </div>
    <div class="stat">
      <div class="sv">${fmt(m.gas, "", 0)}</div>
      <div class="sl">Smoke (${m.smokeLevel || "—"})</div>
    </div>
    <div class="stat">
      <div class="sv">${dist}</div>
      <div class="sl">To Leader</div>
    </div>
  `;

  hints.innerHTML = `
    <div class="hint"><b>Last update:</b> ${lastSeen}</div>
    <div class="hint"><b>Status:</b> ${severityLabel(m.severity)} • <b>Worst:</b> ${severityLabel(m.worst || "ok")}</div>
  `;

  if (isNum(m.lat) && isNum(m.lon)) {
    mapFocus.setView([m.lat, m.lon], 19);
    markersFocus[id]?.openPopup();
  }
}

/* === MQTT CONNECTION === */
function connectMQTT() {
  const client = mqtt.connect(`ws://${CONFIG.MQTT_HOST}:${CONFIG.MQTT_PORT}/mqtt`);
  const statusDot = document.querySelector(".status-dot");

  client.on("connect", () => {
    statusDot.classList.add("connected");
    document.getElementById("connectionStatus").innerHTML =
      '<span class="status-dot connected"></span> MQTT: Online';
    client.subscribe(CONFIG.TEAM_TOPIC);
    client.subscribe(CONFIG.ALERT_TOPIC);
  });

  client.on("message", (topic, payload) => {
    let msg;
    try { msg = JSON.parse(payload.toString()); } catch { return; }

    if (topic.includes("edge/status")) {
      // Normalize & store
      if (!msg.ffId) return;
      state.members[msg.ffId] = msg;

      updateMarker(map, markers, msg.ffId, msg);
      updateMarker(mapFocus, markersFocus, msg.ffId, msg);

      renderTeamList();
      updateFocusStats();
      updateAlertBox();

      // If someone is danger on marker severity, show gentle toast (not constant)
      if (msg.worst === "danger") {
        showToast("danger", "Critical Condition", `${msg.name} has a danger-level condition.`);
      } else if (msg.worst === "warn") {
        // show warn toast only if very recent and not spamming: rely on alerts topic for details
      }
    }

    if (topic.includes("edge/alerts")) {
      // Alerts packet: { ffId, worst, alerts[] }
      if (!msg.ffId) return;
      state.alertsByFf[msg.ffId] = msg;

      updateAlertBox();

      // Toast only for warn/danger, once per received alert packet
      if (msg.worst === "danger") {
        const first = msg.alerts?.[0];
        const line = first ? `${first.title}: ${first.detail}` : "Danger alert received.";
        showToast("danger", `ALERT – ${msg.name}`, line);
      } else if (msg.worst === "warn") {
        const first = msg.alerts?.[0];
        const line = first ? `${first.title}: ${first.detail}` : "Warning alert received.";
        showToast("warn", `Warning – ${msg.name}`, line);
      }
    }
  });

  // Focus dropdown change
  document.getElementById("focusSelect").addEventListener("change", () => updateFocusStats());

  // Video load
  document.getElementById("btnLoadVideo").addEventListener("click", () => {
    const url = document.getElementById("videoUrl").value.trim();
    const frame = document.getElementById("videoFrame");
    const ph = document.getElementById("videoPlaceholder");
    if (!url) return;

    frame.src = url;
    frame.classList.remove("hidden");
    ph.classList.add("hidden");
  });
}

/* === TABS === */
window.switchTab = (tabId) => {
  document.querySelectorAll(".tab-pane").forEach(el => el.classList.remove("active"));
  document.querySelectorAll(".tab-btn").forEach(el => el.classList.remove("active"));

  document.getElementById("tab-" + tabId).classList.add("active");
  event.currentTarget.classList.add("active");

  setTimeout(() => {
    map.invalidateSize();
    mapFocus.invalidateSize();
  }, 120);
};

/* === CLOCK === */
function tickClock() {
  const el = document.getElementById("clock");
  const now = new Date();
  el.textContent = now.toLocaleString(undefined, {
    hour: "2-digit", minute: "2-digit", second: "2-digit",
    year: "numeric", month: "short", day: "2-digit"
  });
}
setInterval(tickClock, 1000);
tickClock();

/* === INIT === */
initMaps();
connectMQTT();
