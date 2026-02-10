// =========================================================
// EDGE DASHBOARD — WITH TRAILS + SHOW EVERYONE + INCIDENT RADIUS
// + YOUTUBE SEGMENT PLAYBACK FOR B/C/D (40s loops)
// =========================================================

const CONFIG = {
  MQTT_HOST: window.location.hostname || "localhost",
  MQTT_PORT: 9001,
  TEAM_TOPIC: "edge/status/Team_A/#",
  ALERT_TOPIC: "edge/alerts/Team_A/#",
  INCIDENT_TOPIC: "edge/incident/Team_A"
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

/**
 * ===== CAMERA FEEDS PER FIREFIGHTER =====
 * For YouTube segment loops use: type: "youtube"
 */
const CAMERA_URLS = {
  FF_A: { type: "iframe", url: "http://192.168.2.13:8889/cam" },

  // YouTube segments (video mphHFk5IXsQ):
  // B: 00:10 -> 00:50
  // C: 01:10 -> 01:50
  // D: 02:10 -> 02:50
  FF_B: { type: "youtube", videoId: "mphHFk5IXsQ", start: 10,  duration: 40 },
  FF_C: { type: "youtube", videoId: "mphHFk5IXsQ", start: 70,  duration: 40 },
  FF_D: { type: "youtube", videoId: "mphHFk5IXsQ", start: 130, duration: 40 },
};

function openCamera(ffId){
  const cam = CAMERA_URLS[ffId];
  if (!cam) return;

  if (cam.type === "youtube"){
    const t = cam.start ?? 0;
    window.open(`https://www.youtube.com/watch?v=${cam.videoId}&t=${t}s`, "_blank", "noopener");
    return;
  }

  if (!cam.url) return;
  window.open(cam.url, "_blank", "noopener");
}

const state = {
  members: {},
  alertsByFf: {},
  centeredOnce: false,
  incident: null, // {lat,lon,radiusM,...}
};

let map;
const markers = {};

// Incident layers
let incidentCircle = null;
let incidentHotRing = null;

// =========================================================
// Helpers
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
// INCIDENT DRAWING
// =========================================================
function updateIncidentUI(pkt){
  state.incident = pkt;

  const txt = document.getElementById("incidentText");
  if (txt){
    const lead = isNum(pkt.leadM) ? ` • lead ${Math.round(pkt.leadM)}m` : "";
    txt.textContent = `🔥 r=${Math.round(pkt.radiusM)}m${lead}`;
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
// SHOW EVERYONE (fit bounds) — includes incident if available
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
// YOUTUBE SEGMENT PLAYER (B/C/D)
// =========================================================
const ytPlayers = {};        // ffId -> YT.Player
const ytSegments = {};       // ffId -> {start,end,videoId}
let ytApiReady = false;
let ytTick = null;

// Called by YouTube API automatically (global)
window.onYouTubeIframeAPIReady = () => {
  ytApiReady = true;
  tryInitAllYouTubePlayers();
};

function segmentFor(ffId){
  const cam = CAMERA_URLS[ffId];
  if (!cam || cam.type !== "youtube") return null;
  const start = cam.start ?? 0;
  const end = start + (cam.duration ?? 40);
  return { start, end, videoId: cam.videoId };
}

function showTapOverlay(ffId){
  const el = document.getElementById(`tap-${ffId}`);
  if (el) el.style.display = "flex";
}
function hideTapOverlay(ffId){
  const el = document.getElementById(`tap-${ffId}`);
  if (el) el.style.display = "none";
}

function userPlay(ffId){
  const p = ytPlayers[ffId];
  const s = ytSegments[ffId];
  if (!p || !s) return;

  try { p.mute(); } catch {}
  try { p.seekTo(s.start, true); } catch {}
  try { p.playVideo(); } catch {}

  hideTapOverlay(ffId);
}

function tryInitAllYouTubePlayers(){
  if (!ytApiReady) return;

  for (const ffId of FF_ORDER){
    const cam = CAMERA_URLS[ffId];
    if (cam?.type !== "youtube") continue;

    const host = document.getElementById(`yt-${ffId}`);
    if (!host) continue;

    // If DOM got re-rendered, the old player is attached to a removed element.
    // Safest: destroy and recreate when the host is new.
    if (ytPlayers[ffId]){
      // If the old player's iframe is not inside the current host, rebuild.
      try {
        const iframe = ytPlayers[ffId].getIframe?.();
        if (iframe && !host.contains(iframe)){
          ytPlayers[ffId].destroy?.();
          delete ytPlayers[ffId];
        }
      } catch {
        // ignore
      }
    }

    if (ytPlayers[ffId]) continue;

    const seg = segmentFor(ffId);
    if (!seg) continue;

    ytSegments[ffId] = seg;

    ytPlayers[ffId] = new YT.Player(`yt-${ffId}`, {
      videoId: seg.videoId,
      playerVars: {
        autoplay: 1,
        controls: 0,
        mute: 1,
        rel: 0,
        playsinline: 1,
        modestbranding: 1,
        fs: 0,
        iv_load_policy: 3,
        start: seg.start,
        loop: 1,
        playlist: seg.videoId,
      },
      events: {
        onReady: (e) => {
          const player = e.target;

          try { player.mute(); } catch {}
          try { player.seekTo(seg.start, true); } catch {}

          // Many browsers need a short delay after ready
          setTimeout(() => {
            try { player.playVideo(); } catch {}
          }, 300);

          ensureYtTicker();
          hideTapOverlay(ffId);
        },
        onStateChange: (e) => {
          const player = e.target;
          const s = ytSegments[ffId];
          if (!s) return;

          if (e.data === YT.PlayerState.ENDED){
            try { player.seekTo(s.start, true); player.playVideo(); } catch {}
            return;
          }

          // Autoplay blocked often results in PAUSED state
          if (e.data === YT.PlayerState.PAUSED){
            showTapOverlay(ffId);
          }

          if (e.data === YT.PlayerState.PLAYING){
            hideTapOverlay(ffId);
          }
        }
      }
    });
  }
}

function ensureYtTicker(){
  if (ytTick) return;
  ytTick = setInterval(() => {
    for (const [ffId, player] of Object.entries(ytPlayers)){
      const seg = ytSegments[ffId];
      if (!seg) continue;

      try {
        const t = player.getCurrentTime?.();
        const st = player.getPlayerState?.();

        // If not playing, we don't force-play too aggressively (browser policies),
        // but we do keep it in bounds when it *is* playing.
        if (typeof t === "number"){
          if (t < seg.start - 0.3 || t >= seg.end - 0.2){
            player.seekTo?.(seg.start, true);
            if (st === YT.PlayerState.PLAYING || st === YT.PlayerState.BUFFERING){
              player.playVideo?.();
            }
          }
        }
      } catch {
        // ignore
      }
    }
  }, 500);
}

// =========================================================
// UNIT CARDS
// =========================================================
function kpiClass(level){
  if (level === "Critical" || level === "Danger" || level === "Extreme") return "danger";
  if (level === "High" || level === "Hot") return "warn";
  return "";
}

function cameraHtml(ffId){
  const cam = CAMERA_URLS[ffId];
  if (!cam){
    return `<div class="cam cam-empty">No camera</div>`;
  }

  if (cam.type === "youtube"){
    return `
      <div class="cam cam-yt" data-ff="${ffId}" title="Click to open">
        <div id="yt-${ffId}"></div>

        <div id="tap-${ffId}" class="cam-tap" style="display:none">
          <button class="cam-tap-btn">Tap to play</button>
        </div>

        <div class="cam-overlay">LIVE</div>
      </div>
    `;
  }

  if (!cam.url){
    return `<div class="cam cam-empty">No camera</div>`;
  }

  if (cam.type === "iframe"){
    return `
      <div class="cam" data-ff="${ffId}" title="Click to open">
        <iframe class="cam-iframe" src="${cam.url}" loading="lazy" referrerpolicy="no-referrer"></iframe>
        <div class="cam-overlay">LIVE</div>
      </div>
    `;
  }

  return `
    <div class="cam" data-ff="${ffId}" title="Click to open">
      <img class="cam-img" src="${cam.url}" alt="Camera ${ffId}" />
      <div class="cam-overlay">LIVE</div>
    </div>
  `;
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
      ${cameraHtml(ffId)}
      <div class="kpis">
        <div class="kpi"><div class="v">—</div><div class="l">Pulse</div></div>
        <div class="kpi"><div class="v">—</div><div class="l">Temp</div></div>
        <div class="kpi"><div class="v">—</div><div class="l">Smoke</div></div>
      </div>
    `;
  } else {
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

      ${cameraHtml(ffId)}

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
  }

  // Click card -> zoom map to unit + open popup
  el.onclick = (ev) => {
    const camEl = ev.target?.closest?.(".cam");
    if (camEl) return;

    const m2 = state.members[ffId];
    if (m2 && isNum(m2.lat) && isNum(m2.lon)){
      map.setView([m2.lat, m2.lon], 19);
      markers[ffId]?.openPopup();
    }
  };

  // Click camera -> either play YT (user gesture) or open stream
  const camEl = el.querySelector(".cam");
  if (camEl){
    camEl.addEventListener("click", (e) => {
      e.stopPropagation();

      if (CAMERA_URLS[ffId]?.type === "youtube"){
        userPlay(ffId);   // user gesture enables autoplay
        return;
      }

      openCamera(ffId);
    });
  }

  // After rendering, init YT players if needed (cards are re-rendered often)
  if (CAMERA_URLS[ffId]?.type === "youtube"){
    tryInitAllYouTubePlayers();
  }
}

function renderAllCards(){
  FF_ORDER.forEach(renderUnitCard);
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

      scheduleRender();
      maybeCenterOnLeader();
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

  renderAllCards();
  renderAlerts();
}

initMap();
connectMQTT();
