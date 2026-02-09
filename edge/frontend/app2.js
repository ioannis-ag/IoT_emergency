const CONFIG = {
  MQTT_HOST: window.location.hostname || "localhost",
  MQTT_PORT: 9001,
  TEAM_TOPIC: "edge/status/Team_A/#",
  ALERT_TOPIC: "edge/alerts/Team_A/#"
};

const FF_ORDER = ["FF_A", "FF_B", "FF_C", "FF_D"];

const state = {
  members: {},        // ffId -> summary
  alertsByFf: {},     // ffId -> last alerts packet
  centeredOnce: false
};

let map;
const markers = {};

function isNum(x){ return typeof x === "number" && Number.isFinite(x); }
function fmt(x, d=0){ return isNum(x) ? x.toFixed(d) : "—"; }
function sevRank(s){ return ({ok:0, warn:1, danger:2}[s] ?? 0); }

function tickClock(){
  const el = document.getElementById("clock");
  if (!el) return;
  el.textContent = new Date().toLocaleString(undefined, {
    hour:"2-digit", minute:"2-digit", second:"2-digit",
    day:"2-digit", month:"short", year:"numeric"
  });
}
setInterval(tickClock, 1000);
tickClock();

/* MARKERS */
function markerIcon(sev="ok"){
  const cls = sev === "danger" ? "ff-danger" : (sev === "warn" ? "ff-warn" : "ff-ok");
  return L.divIcon({
    className: "",
    html: `<div class="ff-marker-wrap ${cls}"></div>`,
    iconSize: [18,18],
    iconAnchor: [9,9]
  });
}

function popupHtml(m){
  const dist = isNum(m.distanceToLeaderM) ? `${Math.round(m.distanceToLeaderM)} m` : "—";
  const seen = (m.lastSeenSec ?? null) === null ? "—" : `${m.lastSeenSec}s ago`;
  return `
    <div style="min-width:240px">
      <div style="font-weight:1100; font-size:14px;">${m.name} <span style="color:#54657e;font-weight:900">(${m.ffId})</span></div>
      <div style="margin-top:8px; color:#54657e; font-size:13px; line-height:1.45;">
        ❤️ Pulse: <b>${fmt(m.pulse,0)}</b> <span style="opacity:.85">(${m.pulseLevel||"—"})</span><br/>
        🌡️ Temp: <b>${fmt(m.temp,1)}°C</b> <span style="opacity:.85">(${m.heatLevel||"—"})</span><br/>
        🫁 Smoke: <b>${fmt(m.gas,0)}</b> <span style="opacity:.85">(${m.smokeLevel||"—"})</span><br/>
        📍 Dist: <b>${dist}</b><br/>
        ⏱ Last: <b>${seen}</b>
      </div>
    </div>
  `;
}

function upsertMarker(m){
  if (!isNum(m.lat) || !isNum(m.lon)) return;
  const id = m.ffId;

  if (!markers[id]){
    const mk = L.marker([m.lat, m.lon], { icon: markerIcon(m.severity) }).addTo(map);
    mk.bindPopup(popupHtml(m));
    mk.bindTooltip(m.name, { direction:"top", offset:[0,-10], opacity:0.9 });
    markers[id] = mk;
  } else {
    markers[id].setLatLng([m.lat, m.lon]);
    markers[id].setIcon(markerIcon(m.severity));
    markers[id].getPopup().setContent(popupHtml(m));
    markers[id].setTooltipContent(m.name);
  }
}

/* AUTO-CENTER ON LEADER */
function maybeCenterOnLeader(){
  const a = state.members["FF_A"];
  if (!a || !isNum(a.lat) || !isNum(a.lon)) return;

  if (!state.centeredOnce){
    map.setView([a.lat, a.lon], 19);
    state.centeredOnce = true;
  }
}

/* UNIT CARDS */
function kpiClass(level){
  if (level === "Critical" || level === "Danger" || level === "Extreme") return "danger";
  if (level === "High" || level === "Hot") return "warn";
  return "";
}

function renderUnitCard(ffId){
  const el = document.getElementById(`card-${ffId}`);
  if (!el) return;

  const m = state.members[ffId];

  if (!m){
    el.innerHTML = `
      <div class="unit-top">
        <div class="unit-name">${ffId}</div>
        <span class="pill stale">WAITING</span>
      </div>
      <div class="unit-meta">No data yet</div>
      <div class="kpis">
        <div class="kpi"><div class="v">—</div><div class="l">Pulse</div></div>
        <div class="kpi"><div class="v">—</div><div class="l">Temp</div></div>
        <div class="kpi"><div class="v">—</div><div class="l">Smoke</div></div>
      </div>
    `;
    return;
  }

  const stale = (m.lastSeenSec ?? 9999) > 12;
  const pill = stale
    ? `<span class="pill stale">STALE</span>`
    : `<span class="pill ${m.severity}">${(m.severity||"ok").toUpperCase()}</span>`;

  const dist = isNum(m.distanceToLeaderM) ? `${Math.round(m.distanceToLeaderM)}m` : "—";
  const last = (m.lastSeenSec ?? null) === null ? "—" : `${m.lastSeenSec}s`;

  el.innerHTML = `
    <div class="unit-top">
      <div class="unit-name">${m.name}</div>
      ${pill}
    </div>
    <div class="unit-meta">
      <span>${m.ffId}</span>
      <span>⏱ ${last}</span>
      <span>📍 ${dist}</span>
    </div>

    <div class="kpis">
      <div class="kpi ${kpiClass(m.pulseLevel)}">
        <div class="v">${fmt(m.pulse,0)}</div>
        <div class="l">Pulse</div>
      </div>
      <div class="kpi ${kpiClass(m.heatLevel)}">
        <div class="v">${fmt(m.temp,1)}°C</div>
        <div class="l">Temp</div>
      </div>
      <div class="kpi ${kpiClass(m.smokeLevel)}">
        <div class="v">${fmt(m.gas,0)}</div>
        <div class="l">Smoke</div>
      </div>
    </div>
  `;

  // click card -> zoom map to unit + open popup
  el.onclick = () => {
    if (isNum(m.lat) && isNum(m.lon)){
      map.setView([m.lat, m.lon], 19);
      markers[m.ffId]?.openPopup();
    }
  };
}

function renderAllCards(){
  FF_ORDER.forEach(renderUnitCard);
}

/* ALERTS (BOTTOM PANEL ONLY — NO TOP-RIGHT TOASTS) */
function worstOverall(){
  let w = "ok";
  for (const p of Object.values(state.alertsByFf)){
    if (!p) continue;
    if (sevRank(p.worst) > sevRank(w)) w = p.worst;
  }
  return w;
}

function updateMissionBadge(){
  const worst = worstOverall();
  const badge = document.getElementById("missionBadge");
  const panel = document.getElementById("alertsPanel");
  const sub = document.getElementById("alertsSub");

  if (badge){
    badge.classList.remove("ok","warn","danger");
    badge.classList.add(worst);
    badge.textContent = worst === "danger" ? "Danger" : worst === "warn" ? "Warning" : "Nominal";
  }

  if (panel){
    panel.classList.remove("ok","warn","danger");
    panel.classList.add(worst);
  }

  if (sub){
    sub.textContent =
      worst === "danger" ? "Immediate attention required" :
      worst === "warn" ? "Monitor active risks" :
      "No active alerts";
  }
}

function renderAlerts(){
  const content = document.getElementById("alertContent");
  if (!content) return;

  const packets = Object.values(state.alertsByFf)
    .filter(p => p && Array.isArray(p.alerts) && p.alerts.length)
    .sort((a,b) => sevRank(b.worst)-sevRank(a.worst));

  content.innerHTML = "";

  if (!packets.length){
    content.innerHTML = `<div class="empty">No active alerts.</div>`;
    updateMissionBadge();
    return;
  }

  // Show up to 2 units with alerts, up to 4 alerts each
  packets.slice(0,2).forEach(p => {
    const block = document.createElement("div");
    block.className = "alert-block";

    block.innerHTML = `
      <div class="alert-head2">
        <div class="alert-name">${p.name || p.ffId}</div>
        <span class="pill ${p.worst}">${p.worst.toUpperCase()}</span>
      </div>
      <div class="alert-meta">Active conditions</div>
    `;

    p.alerts.slice(0,4).forEach(a => {
      const item = document.createElement("div");
      item.className = `alert-item ${a.severity}`;
      item.innerHTML = `<div class="t">${a.title}</div><div class="d">${a.detail}</div>`;
      block.appendChild(item);
    });

    content.appendChild(block);
  });

  updateMissionBadge();
}

/* MQTT */
function connectMQTT(){
  const client = mqtt.connect(`ws://${CONFIG.MQTT_HOST}:${CONFIG.MQTT_PORT}/mqtt`);

  client.on("connect", () => {
    const dot = document.querySelector(".dot");
    const text = document.querySelector(".conn-text");
    if (dot) dot.classList.add("on");
    if (text) text.textContent = "MQTT Online";

    client.subscribe(CONFIG.TEAM_TOPIC);
    client.subscribe(CONFIG.ALERT_TOPIC);
  });

  client.on("message", (topic, payload) => {
    let msg;
    try { msg = JSON.parse(payload.toString()); } catch { return; }

    if (topic.includes("edge/status")){
      if (!msg.ffId) return;

      state.members[msg.ffId] = msg;
      upsertMarker(msg);

      renderAllCards();
      maybeCenterOnLeader();
    }

    if (topic.includes("edge/alerts")){
      if (!msg.ffId) return;

      state.alertsByFf[msg.ffId] = msg;
      renderAlerts();
      // intentionally NO toast popups here
    }
  });
}

/* INIT */
function initMap(){
  map = L.map("mainMap").setView([38.2466, 21.7346], 19);
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
    maxZoom: 20,
    attribution: "© OpenStreetMap"
  }).addTo(map);

  const btn = document.getElementById("btnRecenter");
  if (btn){
    btn.onclick = () => {
      const a = state.members["FF_A"];
      if (a && isNum(a.lat) && isNum(a.lon)) map.setView([a.lat, a.lon], 19);
    };
  }

  renderAllCards();
  renderAlerts();
}

initMap();
connectMQTT();
