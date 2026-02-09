(() => {
  const CFG = {
    TEAM: "Team_A",
    MQTT_WS_HOST: "192.168.2.13",
    MQTT_WS_PORT: 9001,
    TOPICS: ["edge/status/#", "edge/alerts/#", "ngsi/Location/#"],
    MAP_CENTER: [38.2466, 21.7346],
    MAP_ZOOM: 14,
  };

  const state = { byFF: new Map(), order: [], focusedKey: null };

  const mqttStatusEl = document.getElementById("mqttStatus");
  const lastMsgEl = document.getElementById("lastMsg");
  const focusedNameEl = document.getElementById("focusedName");
  const rosterEl = document.getElementById("roster");
  const drawerTitleEl = document.getElementById("drawerTitle");
  const drawerGridEl = document.getElementById("drawerGrid");
  const toastStackEl = document.getElementById("toastStack");
  const searchBoxEl = document.getElementById("searchBox");
  const focusSelectOpsEl = document.getElementById("focusSelectOps");
  const focusSelectEl = document.getElementById("focusSelect");

  const focusNameEl = document.getElementById("focusName");
  const focusDotEl = document.getElementById("focusDot");
  const focusStatusEl = document.getElementById("focusStatus");
  const focusLastSeenEl = document.getElementById("focusLastSeen");
  const focusGridEl = document.getElementById("focusGrid");

  const camUrlEl = document.getElementById("camUrl");
  const camApplyEl = document.getElementById("camApply");
  const camFrameEl = document.getElementById("camFrame");
  const camImgEl = document.getElementById("camImg");
  const camStatusEl = document.getElementById("camStatus");

  // Tabs
  const tabBtns = Array.from(document.querySelectorAll(".tabbtn[data-tab]"));
  tabBtns.forEach(btn => btn.addEventListener("click", () => {
    tabBtns.forEach(b => b.classList.toggle("active", b === btn));
    const tab = btn.dataset.tab;
    ["ops","focus","video"].forEach(t => document.getElementById(`view_${t}`).classList.toggle("active", t === tab));
    if (tab === "ops") setTimeout(() => map.invalidateSize(), 50);
    if (tab === "focus") setTimeout(() => miniMap.invalidateSize(), 50);
  }));

  // Maps
  const map = L.map("map").setView(CFG.MAP_CENTER, CFG.MAP_ZOOM);
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", { maxZoom: 19 }).addTo(map);

  const miniMap = L.map("miniMap").setView(CFG.MAP_CENTER, 15);
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", { maxZoom: 19 }).addTo(miniMap);
  const miniMarker = L.marker(CFG.MAP_CENTER).addTo(miniMap);

  const markers = new Map();
  const mkKey = (team, ff) => `${team}|${ff}`;
  const prettyKey = (key) => key.split("|")[1] || key;

  const sevClass = (sev) => sev === "danger" ? "danger" : sev === "warn" ? "warn" : sev === "offline" ? "offline" : "ok";

  const fmtAgo = (sec) => {
    if (sec == null || isNaN(sec)) return "–";
    if (sec < 60) return `${sec}s`;
    const m = Math.floor(sec/60), s = sec % 60;
    if (m < 60) return `${m}m ${s}s`;
    const h = Math.floor(m/60);
    return `${h}h ${m%60}m`;
  };

  const pushToast = (alert) => {
    const sev = alert.severity || "info";
    const div = document.createElement("div");
    div.className = `toast ${sevClass(sev)}`;
    div.innerHTML = `
      <div class="toastTitle">${sev.toUpperCase()} • ${alert.ffId || "?"}</div>
      <p class="toastBody">${alert.category || "alert"} — ${alert.reason || ""}${alert.value != null ? ` (${alert.value})` : ""}</p>
      <div class="toastTime">${new Date().toLocaleTimeString()}</div>
    `;
    toastStackEl.prepend(div);
    setTimeout(() => {
      div.style.opacity = "0";
      div.style.transform = "translateY(-4px)";
      div.style.transition = "all .25s ease";
      setTimeout(() => div.remove(), 260);
    }, 6500);
  };

  const mkIcon = (sev, headingDeg) => {
    const cls = sevClass(sev);
    const html = `
      <div class="ffMarker ${cls}" style="transform: rotate(${headingDeg || 0}deg)">
        <div class="arrow"></div>
      </div>`;
    return L.divIcon({ html, className:"", iconSize:[28,28], iconAnchor:[14,14] });
  };

  const computeHeading = (prev, cur) => {
    if (!prev || !cur) return null;
    const [lat1, lon1] = prev, [lat2, lon2] = cur;
    const toRad = d => d * Math.PI / 180;
    const y = Math.sin(toRad(lon2-lon1)) * Math.cos(toRad(lat2));
    const x = Math.cos(toRad(lat1))*Math.sin(toRad(lat2)) - Math.sin(toRad(lat1))*Math.cos(toRad(lat2))*Math.cos(toRad(lon2-lon1));
    let brng = Math.atan2(y,x) * 180/Math.PI;
    return (brng + 360) % 360;
  };

  const upsertMarker = (key, lat, lon, sev) => {
    if (typeof lat !== "number" || typeof lon !== "number") return;
    const entry = state.byFF.get(key) || {};
    const prevLL = entry.lastLatLng || null;
    const curLL = [lat, lon];
    const heading = computeHeading(prevLL, curLL) ?? entry.headingDeg ?? 0;
    entry.headingDeg = heading;
    entry.lastLatLng = curLL;
    state.byFF.set(key, entry);

    const icon = mkIcon(sev, heading);

    if (!markers.has(key)){
      const m = L.marker(curLL, { icon }).addTo(map);
      m.on("click", () => selectFF(key, true));
      markers.set(key, { marker: m });
    } else {
      markers.get(key).marker.setLatLng(curLL);
      markers.get(key).marker.setIcon(icon);
    }

    if (state.focusedKey === key){
      map.panTo(curLL, { animate: true });
      miniMap.panTo(curLL, { animate: true });
      miniMarker.setLatLng(curLL);
    }
  };

  const renderRoster = () => {
    const q = (searchBoxEl.value || "").trim().toLowerCase();
    rosterEl.innerHTML = "";
    state.order.filter(k => !q || prettyKey(k).toLowerCase().includes(q)).forEach(key => {
      const entry = state.byFF.get(key) || {};
      const s = entry.summary || {};
      const sev = s.severity || "offline";
      const status = s.status || "unknown";
      const card = document.createElement("div");
      card.className = "card" + (state.focusedKey === key ? " active" : "");
      card.addEventListener("click", () => selectFF(key, true));
      card.innerHTML = `
        <div class="row">
          <div>
            <div class="name">${prettyKey(key)}</div>
            <div class="meta">status: <b>${status}</b> • last seen: ${fmtAgo(s.lastSeenSec)}</div>
          </div>
          <div class="sev ${sevClass(sev)}"><span class="dot"></span>${sev}</div>
        </div>
        <div class="pills">
          <div class="pill">HR <strong>${s.hrBpm ?? "–"}</strong> bpm</div>
          <div class="pill">Temp <strong>${s.tempC != null ? Number(s.tempC).toFixed(1) : "–"}</strong> °C</div>
          <div class="pill">MQ2 <strong>${s.mq2Raw ?? "–"}</strong></div>
        </div>`;
      rosterEl.appendChild(card);
    });

    const fillSelect = (sel) => {
      const cur = sel.value;
      sel.innerHTML = `<option value="">${sel.id === "focusSelectOps" ? "Focus…" : "Select firefighter…"}</option>`;
      state.order.forEach(k => {
        const opt = document.createElement("option");
        opt.value = k; opt.textContent = prettyKey(k);
        sel.appendChild(opt);
      });
      if (cur) sel.value = cur;
    };
    fillSelect(focusSelectOpsEl);
    fillSelect(focusSelectEl);
  };

  const renderDrawer = (key) => {
    const entry = state.byFF.get(key) || {};
    const s = entry.summary || {};
    drawerTitleEl.textContent = `${prettyKey(key)} • ${s.status || "unknown"}`;
    focusedNameEl.textContent = prettyKey(key);

    const kvs = [
      ["Heart rate", s.hrBpm, "bpm"],
      ["Temperature", s.tempC != null ? Number(s.tempC).toFixed(1) : null, "°C"],
      ["MQ2", s.mq2Raw, ""],
      ["CO", s.coPpm != null ? Number(s.coPpm).toFixed(1) : null, "ppm"],
      ["Last seen", s.lastSeenSec != null ? fmtAgo(s.lastSeenSec) : null, ""],
      ["Accuracy", s.accuracyM != null ? Math.round(s.accuracyM) : null, "m"],
    ];
    drawerGridEl.innerHTML = "";
    kvs.forEach(([k,v,u]) => {
      const div = document.createElement("div");
      div.className = "kv";
      div.innerHTML = `<div class="k">${k}</div><div class="v">${v ?? "–"}<span class="u">${u}</span></div>`;
      drawerGridEl.appendChild(div);
    });
  };

  const renderFocus = (key) => {
    const entry = state.byFF.get(key) || {};
    const s = entry.summary || {};
    focusNameEl.textContent = prettyKey(key);
    focusStatusEl.textContent = `${s.status || "unknown"} (${s.severity || "offline"})`;
    focusLastSeenEl.textContent = `last seen: ${fmtAgo(s.lastSeenSec)}`;
    focusDotEl.className = `bigDot ${sevClass(s.severity || "offline")}`;

    const kvs = [
      ["Lat", s.lat != null ? Number(s.lat).toFixed(6) : null, ""],
      ["Lon", s.lon != null ? Number(s.lon).toFixed(6) : null, ""],
      ["HR", s.hrBpm, "bpm"],
      ["Temp", s.tempC != null ? Number(s.tempC).toFixed(1) : null, "°C"],
      ["MQ2", s.mq2Raw, ""],
      ["CO", s.coPpm != null ? Number(s.coPpm).toFixed(1) : null, "ppm"],
      ["Accuracy", s.accuracyM != null ? Math.round(s.accuracyM) : null, "m"],
      ["LastSeen", s.lastSeenSec != null ? fmtAgo(s.lastSeenSec) : null, ""],
    ];
    focusGridEl.innerHTML = "";
    kvs.forEach(([k,v,u]) => {
      const div = document.createElement("div");
      div.className = "kv";
      div.innerHTML = `<div class="k">${k}</div><div class="v">${v ?? "–"}<span class="u">${u}</span></div>`;
      focusGridEl.appendChild(div);
    });

    if (typeof s.lat === "number" && typeof s.lon === "number"){
      const ll = [s.lat, s.lon];
      miniMarker.setLatLng(ll);
      miniMap.setView(ll, 16, { animate:true });
    }
  };

  const selectFF = (key, center) => {
    if (!key) return;
    state.focusedKey = key;
    renderRoster();
    renderDrawer(key);
    renderFocus(key);
    const s = (state.byFF.get(key) || {}).summary || {};
    if (center && typeof s.lat === "number" && typeof s.lon === "number"){
      const ll = [s.lat, s.lon];
      map.setView(ll, 16, { animate:true });
      miniMap.setView(ll, 16, { animate:true });
      miniMarker.setLatLng(ll);
    }
  };

  focusSelectOpsEl.addEventListener("change", () => selectFF(focusSelectOpsEl.value, true));
  focusSelectEl.addEventListener("change", () => selectFF(focusSelectEl.value, true));
  searchBoxEl.addEventListener("input", renderRoster);

  // MQTT
  const client = mqtt.connect(`ws://${CFG.MQTT_WS_HOST}:${CFG.MQTT_WS_PORT}`);
  client.on("connect", () => { mqttStatusEl.textContent = "connected"; CFG.TOPICS.forEach(t => client.subscribe(t)); });
  client.on("close", () => mqttStatusEl.textContent = "disconnected");
  client.on("error", () => mqttStatusEl.textContent = "error");

  const ensureKey = (team, ff) => {
    const key = mkKey(team, ff);
    if (!state.byFF.has(key)) state.byFF.set(key, {});
    if (!state.order.includes(key)) state.order.push(key);
    return key;
  };

  client.on("message", (topic, msg) => {
    let data; try { data = JSON.parse(msg.toString()); } catch { return; }
    lastMsgEl.textContent = topic.split("/").slice(0,3).join("/") + " • " + new Date().toLocaleTimeString();

    if (topic.startsWith("edge/status/")){
      const [_, __, team, ff] = topic.split("/");
      const key = ensureKey(team, ff);
      const entry = state.byFF.get(key) || {};
      entry.summary = data;
      state.byFF.set(key, entry);

      upsertMarker(key, data.lat, data.lon, data.severity || "offline");

      if (!state.focusedKey){ state.focusedKey = key; selectFF(key, false); }
      else if (state.focusedKey === key){ renderDrawer(key); renderFocus(key); }

      renderRoster();
      return;
    }

    if (topic.startsWith("edge/alerts/")){
      const [_, __, team, ff] = topic.split("/");
      const key = ensureKey(team, ff);
      const entry = state.byFF.get(key) || {};
      entry.lastAlert = data;
      state.byFF.set(key, entry);

      pushToast(data);

      const s = entry.summary || {};
      upsertMarker(key, s.lat, s.lon, data.severity || "warn");

      renderRoster();
      if (state.focusedKey === key){ renderDrawer(key); renderFocus(key); }
      return;
    }

    if (topic.startsWith("ngsi/Location/")){
      const [_, __, team, ff] = topic.split("/");
      const key = ensureKey(team, ff);
      const entry = state.byFF.get(key) || {};
      entry.summary = entry.summary || {};
      entry.summary.lat = data.lat;
      entry.summary.lon = data.lon;
      entry.summary.accuracyM = data.accuracyM ?? data.acc ?? entry.summary.accuracyM;
      entry.summary.severity = entry.summary.severity || "ok";
      state.byFF.set(key, entry);

      upsertMarker(key, entry.summary.lat, entry.summary.lon, entry.summary.severity);
      renderRoster();
      if (state.focusedKey === key){ renderDrawer(key); renderFocus(key); }
    }
  });

  // Video
  const loadCamera = (url) => {
    camStatusEl.textContent = "loading…";
    camFrameEl.style.display = "block";
    camImgEl.style.display = "none";

    let iframeOk = false;
    camFrameEl.onload = () => { iframeOk = true; camStatusEl.textContent = "live"; };
    camFrameEl.src = url;

    setTimeout(() => {
      if (!iframeOk){
        camFrameEl.style.display = "none";
        camImgEl.style.display = "block";
        camImgEl.src = url;
        camStatusEl.textContent = "live (img/mjpeg)";
      }
    }, 1800);
  };

  camApplyEl.addEventListener("click", () => loadCamera(camUrlEl.value.trim()));
  loadCamera(camUrlEl.value.trim());

  setTimeout(() => map.invalidateSize(), 200);
  setTimeout(() => miniMap.invalidateSize(), 200);
})();
