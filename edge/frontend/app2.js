// =========================================================
// EDGE DASHBOARD — STABLE CAMERAS (NO RE-RENDER BREAKAGE)
// + TRAILS + SHOW EVERYONE + INCIDENT RADIUS + YT SEGMENTS
// =========================================================

const CONFIG = {
  MQTT_HOST: window.location.hostname || "localhost",
  MQTT_PORT: 9001,
  TEAM_TOPIC: "edge/status/Team_A/#",
  ALERT_TOPIC: "edge/alerts/Team_A/#",
  INCIDENT_TOPIC: "edge/incident/Team_A",
};

const FF_ORDER = ["FF_A", "FF_B", "FF_C", "FF_D"];

// ===== TRAILS CONFIG =====
const TRAIL_POINTS = 10;
const TRAIL_MIN_METERS = 3;
const TRAIL_OPTS = { weight: 4, opacity: 0.75 };
const trails = {}; // ffId -> { points: [[lat,lon],...], line: L.Polyline }

// ===== Render throttle =====
let renderTimer = null;
function scheduleRender(){
  if (renderTimer) return;
  renderTimer = setTimeout(() => {
    renderTimer = null;
    renderAllCards();
  }, 200);
}

// =========================================================
// CAMERA CONFIG
// - FF_A uses iframe (your mtX / mtxmedia endpoint)
// - FF_B/C/D use YouTube segments with loop windows
// =========================================================
const YT_VIDEO_ID = "mphHFk5IXsQ";
const CAMERA_URLS = {
  FF_A: { type: "iframe", url: "http://192.168.2.13:8889/cam" },

  // YouTube segments:
  FF_B: { type: "youtube", videoId: YT_VIDEO_ID, start: 10,  dur: 40 },
  FF_C: { type: "youtube", videoId: YT_VIDEO_ID, start: 70,  dur: 40 },  // 1:10
  FF_D: { type: "youtube", videoId: YT_VIDEO_ID, start: 130, dur: 40 },  // 2:10
};

// Optional: click to open full feed in new tab
function openCamera(ffId){
  const cam = CAMERA_URLS[ffId];
  if (!cam) return;
  if (cam.type === "youtube"){
    const url = `https://www.youtube.com/watch?v=${cam.videoId}&t=${cam.start || 0}s`;
    window.open(url, "_blank", "noopener");
  } else if (cam.url){
    window.open(cam.url, "_blank", "noopener");
  }
}

// =========================================================
// STATE
// =========================================================
const state = {
  members: {},        // ffId -> summary
  alertsByFf: {},     // ffId -> alerts packet
  centeredOnce: false,
  incident: null,     // {lat,lon,radiusM,...}
};

let map;
const markers = {};

// Incident layers
let incidentCircle = null;
let incidentHotRing = null;

// =========================================================
// HELPERS
// =========================================================
function isNum(x){ return typeof x === "number" && Number.isFinite(x); }
function fmt(x, d=0){ return isNum(x) ? x.toFixed(d) : "—"; }
function sevRank(s){ return ({ok:0, warn:1, danger:2}[s] ?? 0); }

// =========================================================
// CLOCK
// =========================================================
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

// =========================================================
// TRAILS
// =========================================================
function distanceMeters(a, b){
  const R = 6371000;
  const toRad = x => x * Math.PI / 180;
  const dLat = toRad(b[0] - a[0]);
  const dLon = toRad(b[1] - a[1]);
  const lat1 = toRad(a[0]);
  const lat2 = toRad(b[0]);
  const s = Math.sin(dLat/2)**2 + Math.cos(lat1)*Math.cos(lat2)*Math.sin(dLon/2)**2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(s)));
}

function ensureTrail(ffId){
  if (!trails[ffId]){
    trails[ffId] = {
      points: [],
      line: L.polyline([], TRAIL_OPTS).addTo(map)
    };
  }
  return trails[ffId];
}

function pushTrailPoint(ffId, lat, lon){
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
function updateIncidentUI(pkt){
  state.incident = pkt;

  const txt = document.getElementById("incidentText");
  if (txt){
    const lead = isNum(pkt.leadM) ? ` • lead ${Math.round(pkt.leadM)}m` : "";
    txt.textContent = `r=${Math.round(pkt.radiusM)}m${lead}`;
  }

  if (!map) return;
  const center = [pkt.lat, pkt.lon];

  if (!incidentCircle){
    incidentCircle = L.circle(center, {
      radius: pkt.radiusM,
      weight: 2,
      opacity: 0.9,
      fillOpacity: 0.12
    }).addTo(map);
  } else {
    incidentCircle.setLatLng(center);
    incidentCircle.setRadius(pkt.radiusM);
  }

  const warnRadius = pkt.radiusM + 30;
  if (!incidentHotRing){
    incidentHotRing = L.circle(center, {
      radius: warnRadius,
      weight: 2,
      opacity: 0.5,
      dashArray: "6 8",
      fillOpacity: 0.0
    }).addTo(map);
  } else {
    incidentHotRing.setLatLng(center);
    incidentHotRing.setRadius(warnRadius);
  }
}

// =========================================================
// MARKERS
// =========================================================
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

function upsertMarker(m){
  if (!isNum(m.lat) || !isNum(m.lon)) return;
  const id = m.ffId;

  pushTrailPoint(id, m.lat, m.lon);

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

// =========================================================
// AUTO-CENTER ON LEADER
// =========================================================
function maybeCenterOnLeader(){
  const a = state.members["FF_A"];
  if (!a || !isNum(a.lat) || !isNum(a.lon)) return;

  if (!state.centeredOnce){
    map.setView([a.lat, a.lon], 19);
    state.centeredOnce = true;
  }
}

// =========================================================
// SHOW EVERYONE (fit bounds)
// =========================================================
function fitToEveryone(){
  const pts = [];

  for (const ffId of FF_ORDER){
    const m = state.members[ffId];
    if (m && isNum(m.lat) && isNum(m.lon)) pts.push([m.lat, m.lon]);
  }

  if (state.incident && isNum(state.incident.lat) && isNum(state.incident.lon)){
    pts.push([state.incident.lat, state.incident.lon]);
  }

  if (!pts.length) return;

  if (pts.length === 1){
    map.setView(pts[0], 19);
    return;
  }

  const bounds = L.latLngBounds(pts);
  map.fitBounds(bounds, { padding: [30, 30], maxZoom: 19 });
}

// =========================================================
// KPI CLASS
// =========================================================
function kpiClass(level){
  if (level === "Critical" || level === "Danger" || level === "Extreme") return "danger";
  if (level === "High" || level === "Hot") return "warn";
  return "";
}

// =========================================================
// CAMERA HTML (STATIC — inserted ONCE)
// IMPORTANT: We never re-create the card innerHTML after this,
// otherwise YouTube iframes get destroyed.
// =========================================================
function cameraHtml(ffId){
  const cam = CAMERA_URLS[ffId];
  if (!cam){
    return `<div class="cam cam-empty">No camera</div>`;
  }

  // YouTube host (YT API will replace this div)
  if (cam.type === "youtube"){
    return `
      <div class="cam cam-yt" data-ff="${ffId}">
        <div class="yt-host" id="yt-${ffId}"></div>
      </div>
    `;
  }

  // iframe (FF_A)
  if (cam.type === "iframe"){
    return `
      <div class="cam" data-ff="${ffId}">
        <iframe class="cam-iframe" src="${cam.url}" loading="lazy" referrerpolicy="no-referrer"></iframe>
        <div class="cam-overlay">LIVE</div>
      </div>
    `;
  }

  // mjpeg image
  if (cam.type === "mjpeg"){
    return `
      <div class="cam" data-ff="${ffId}">
        <img class="cam-img" src="${cam.url}" alt="Camera ${ffId}" />
        <div class="cam-overlay">LIVE</div>
      </div>
    `;
  }

  return `<div class="cam cam-empty">No camera</div>`;
}

// =========================================================
// STABLE CARD RENDER (INIT ONCE, UPDATE TEXT ONLY)
// =========================================================
function initCardOnce(ffId){
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

  // Card click -> zoom (but ignore cam clicks)
  el.addEventListener("click", (ev) => {
    const camEl = ev.target?.closest?.(".cam");
    if (camEl) return;
    const m2 = state.members[ffId];
    if (m2 && isNum(m2.lat) && isNum(m2.lon)){
      map.setView([m2.lat, m2.lon], 19);
      markers[ffId]?.openPopup();
    }
  });

  // Cam click -> open new tab
  const cam = el.querySelector(".cam");
  if (cam){
    cam.addEventListener("click", (e) => {
      e.stopPropagation();
      openCamera(ffId);
    });
  }
}

function updateCard(ffId){
  initCardOnce(ffId);

  const m = state.members[ffId];

  const nameEl = document.getElementById(`name-${ffId}`);
  const pillEl = document.getElementById(`pill-${ffId}`);
  const metaEl = document.getElementById(`meta-${ffId}`);

  const pulseEl = document.getElementById(`pulse-${ffId}`);
  const tempEl  = document.getElementById(`temp-${ffId}`);
  const smokeEl = document.getElementById(`smoke-${ffId}`);

  const kpiPulse = document.getElementById(`kpi-pulse-${ffId}`);
  const kpiTemp  = document.getElementById(`kpi-temp-${ffId}`);
  const kpiSmoke = document.getElementById(`kpi-smoke-${ffId}`);

  if (!m){
    if (nameEl) nameEl.textContent = ffId;
    if (pillEl){
      pillEl.className = "pill stale";
      pillEl.textContent = "WAITING";
    }
    if (metaEl) metaEl.innerHTML = `<span>${ffId}</span><span>⏱ —</span><span>📍 —</span>`;
    if (pulseEl) pulseEl.textContent = "—";
    if (tempEl) tempEl.textContent  = "—";
    if (smokeEl) smokeEl.textContent = "—";
    if (kpiPulse) kpiPulse.className = "kpi";
    if (kpiTemp)  kpiTemp.className  = "kpi";
    if (kpiSmoke) kpiSmoke.className = "kpi";
    return;
  }

  const stale = (m.lastSeenSec ?? 9999) > 12;
  if (nameEl) nameEl.textContent = m.name || ffId;

  if (pillEl){
    pillEl.className = stale ? "pill stale" : `pill ${m.severity || "ok"}`;
    pillEl.textContent = stale ? "STALE" : (m.severity || "ok").toUpperCase();
  }

  const dist = isNum(m.distanceToLeaderM) ? `${Math.round(m.distanceToLeaderM)}m` : "—";
  const last = (m.lastSeenSec ?? null) === null ? "—" : `${m.lastSeenSec}s`;
  if (metaEl){
    metaEl.innerHTML = `<span>${m.ffId}</span><span>⏱ ${last}</span><span>📍 ${dist}</span>`;
  }

  if (pulseEl) pulseEl.textContent = isNum(m.pulse) ? `${Math.round(m.pulse)}` : "—";
  if (tempEl)  tempEl.textContent  = isNum(m.temp)  ? `${m.temp.toFixed(1)}°C` : "—";
  if (smokeEl) smokeEl.textContent = isNum(m.gas)   ? `${Math.round(m.gas)}` : "—";

  if (kpiPulse) kpiPulse.className = `kpi ${kpiClass(m.pulseLevel)}`;
  if (kpiTemp)  kpiTemp.className  = `kpi ${kpiClass(m.heatLevel)}`;
  if (kpiSmoke) kpiSmoke.className = `kpi ${kpiClass(m.smokeLevel)}`;
}

function renderAllCards(){
  FF_ORDER.forEach(updateCard);
}

// =========================================================
// ALERTS
// =========================================================
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

// =========================================================
// YOUTUBE IFRAME API — create ONCE and keep playing
// =========================================================
let ytReady = false;
const ytPlayers = {};     // ffId -> YT.Player
const ytLoopTimers = {};  // ffId -> interval id

function loadYouTubeAPI(){
  if (window.YT && window.YT.Player) {
    ytReady = true;
    initAllYouTubePlayers();
    return;
  }
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

function ensureYTPlayer(ffId){
  const cam = CAMERA_URLS[ffId];
  if (!cam || cam.type !== "youtube") return;
  if (!ytReady) return;
  if (ytPlayers[ffId]) return;

  const hostId = `yt-${ffId}`;
  const host = document.getElementById(hostId);
  if (!host) return; // card not built yet

  const start = cam.start ?? 0;
  const end = start + (cam.dur ?? 40);

  ytPlayers[ffId] = new YT.Player(hostId, {
    width: "100%",
    height: "100%",
    videoId: cam.videoId,
    playerVars: {
      autoplay: 1,
      controls: 0,
      mute: 1,           // autoplay needs mute
      playsinline: 1,
      rel: 0,
      modestbranding: 1,
      start: start,
    },
    events: {
      onReady: (e) => {
        try {
          e.target.mute();
          e.target.seekTo(start, true);
          e.target.playVideo();
        } catch {}

        // Loop inside [start, end)
        if (ytLoopTimers[ffId]) clearInterval(ytLoopTimers[ffId]);
        ytLoopTimers[ffId] = setInterval(() => {
          const p = ytPlayers[ffId];
          if (!p || typeof p.getCurrentTime !== "function") return;
          const t = p.getCurrentTime();
          if (t >= end - 0.2) {
            try {
              p.seekTo(start, true);
              p.playVideo();
            } catch {}
          }
        }, 250);
      },
      onStateChange: (e) => {
        // If paused/buffered, try to resume (best effort)
        // 2 = paused, 3 = buffering
        if (e.data === 2 || e.data === 3) {
          const p = e.target;
          try { p.playVideo(); } catch {}
        }
      }
    }
  });
}

function initAllYouTubePlayers(){
  // ensure cards exist first
  renderAllCards();
  // create players for the 3 youtube units
  ["FF_B","FF_C","FF_D"].forEach(ensureYTPlayer);
}

// =========================================================
// MQTT
// =========================================================
function connectMQTT(){
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

    if (topic.includes("edge/status")){
      if (!msg.ffId) return;

      state.members[msg.ffId] = msg;
      upsertMarker(msg);

      // update only text (card is stable)
      scheduleRender();
      maybeCenterOnLeader();

      // after first data, ensure YT players exist
      if (msg.ffId === "FF_B" || msg.ffId === "FF_C" || msg.ffId === "FF_D"){
        ensureYTPlayer(msg.ffId);
      }
    }

    if (topic.includes("edge/alerts")){
      if (!msg.ffId) return;
      state.alertsByFf[msg.ffId] = msg;
      renderAlerts();
    }

    if (topic === CONFIG.INCIDENT_TOPIC){
      if (!isNum(msg.lat) || !isNum(msg.lon) || !isNum(msg.radiusM)) return;
      updateIncidentUI(msg);
    }
  });
}

// =========================================================
// INIT
// =========================================================
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

  const btnAll = document.getElementById("btnShowAll");
  if (btnAll){
    btnAll.onclick = () => fitToEveryone();
  }

  // Build cards once immediately (so YT host divs exist)
  renderAllCards();

  // Start YT API load
  loadYouTubeAPI();

  // Alerts init
  renderAlerts();
}

initMap();
connectMQTT();
