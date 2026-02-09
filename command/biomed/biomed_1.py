#!/usr/bin/env python3
"""
Biomedical MQTT agent (demo-strong):
- Subscribes: raw/ECG/+/+
- Decodes ECG1 bundles + Polar PMD ECG packets (24-bit little-endian samples)
- Filters ECG (notch + bandpass), detects QRS, derives RR series
- Computes: HR, time-domain HRV, freq-domain HRV (LF/HF), nonlinear (SD1/SD2, SampEn),
           Baevsky Stress Index, ectopy/irregularity flags
- Estimates core temperature from HR (demo heuristic) + Physio Strain Index (PSI) + Heat Strain AUC
- Outputs:
    Telemetry/{team}/{ff}            (metrics + indices + risk scores)
    Telemetry/{team}/{ff}/alerts     (alerts array)
Notes:
- This is for operational awareness / demo. Not medical diagnosis.
"""

import os, json, time, struct, logging, math
from dataclasses import dataclass, field
from collections import deque
from typing import Dict, Tuple, Optional, List

import numpy as np
import paho.mqtt.client as mqtt
from scipy.signal import butter, filtfilt, iirnotch, find_peaks, welch

# ---------------- Logging ----------------
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

# ---------------- MQTT ----------------
MQTT_HOST = os.getenv("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
SUB_TOPIC = os.getenv("SUB_TOPIC", "raw/ECG/+/+")

PUB_TELEM_FMT  = os.getenv("PUB_TELEM_FMT",  "Telemetry/{team}/{ff}")
PUB_ALERT_FMT  = os.getenv("PUB_ALERT_FMT",  "Telemetry/{team}/{ff}/alerts")

# ---------------- Signal params ----------------
DEFAULT_FS = float(os.getenv("DEFAULT_FS", "130.0"))          # Polar H10 typically ~130 Hz for PMD ECG
WINDOW_SEC = float(os.getenv("WINDOW_SEC", "30.0"))          # window for HRV
MIN_SEC_FOR_ANALYSIS = float(os.getenv("MIN_SEC_FOR_ANALYSIS", "10.0"))

BANDPASS_LOW_HZ  = float(os.getenv("BANDPASS_LOW_HZ", "0.5"))
BANDPASS_HIGH_HZ = float(os.getenv("BANDPASS_HIGH_HZ", "40.0"))
NOTCH_HZ         = float(os.getenv("NOTCH_HZ", "50.0"))       # 50 Hz EU
NOTCH_Q          = float(os.getenv("NOTCH_Q", "30.0"))

REFRACTORY_SEC   = float(os.getenv("REFRACTORY_SEC", "0.25")) # 240 bpm max
PEAK_PROM_FRAC   = float(os.getenv("PEAK_PROM_FRAC", "0.6"))

# ---------------- Alert thresholds (tune for your demo) ----------------
TACHY_BPM        = float(os.getenv("TACHY_BPM", "170"))
BRADY_BPM        = float(os.getenv("BRADY_BPM", "45"))

LOW_RMSSD_MS     = float(os.getenv("LOW_RMSSD_MS", "18"))     # fatigue/stress proxy
HIGH_LFHF        = float(os.getenv("HIGH_LFHF", "2.5"))       # sympathetic dominance proxy
LOW_SQI          = float(os.getenv("LOW_SQI", "0.30"))

PSI_WARN         = float(os.getenv("PSI_WARN", "6.5"))        # heat strain rising
PSI_DANGER       = float(os.getenv("PSI_DANGER", "7.5"))

IRREGULAR_WARN   = float(os.getenv("IRREGULAR_WARN", "0.18")) # CVRR threshold-ish

# ---------------- Helpers ----------------
def now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

def clamp(x, lo, hi):
    return max(lo, min(hi, x))

def topic_to_team_ff(topic: str) -> Optional[Tuple[str, str]]:
    # raw/ECG/Team_A/FF_A
    parts = topic.split("/")
    if len(parts) >= 4 and parts[0] == "raw" and parts[1] == "ECG":
        return parts[2], parts[3]
    return None

# ---------------- Decoding: ECG1 bundle + Polar PMD ECG ----------------
def s24le_to_int(x3: bytes) -> int:
    b0, b1, b2 = x3[0], x3[1], x3[2]
    v = b0 | (b1 << 8) | (b2 << 16)
    if v & 0x00800000:
        v |= 0xFF000000
    return struct.unpack("<i", struct.pack("<I", v & 0xFFFFFFFF))[0]

def parse_ecg1_bundle(payload: bytes) -> List[bytes]:
    """
    Your edge packs multiple PMD packets into: b"ECG1" + ... + count + [lenLE + pkt]...
    """
    if len(payload) < 9 or payload[0:4] != b"ECG1":
        return []
    count = payload[8]
    idx = 9
    pkts = []
    for _ in range(count):
        if idx + 2 > len(payload):
            break
        L = payload[idx] | (payload[idx + 1] << 8)
        idx += 2
        if idx + L > len(payload):
            break
        pkts.append(payload[idx:idx + L])
        idx += L
    return pkts

def parse_polar_pmd_ecg_packet(pkt: bytes):
    """
    Minimal: [0]=0x00 (type), [1:9]=timestamp ns little-endian,
    then frame-type byte(s), then 24-bit samples.
    Your earlier code uses data = pkt[10:], which matches your on-wire format.
    """
    if len(pkt) < 10 or pkt[0] != 0x00:
        return None, None
    ts = struct.unpack("<Q", pkt[1:9])[0]
    data = pkt[10:]
    n = len(data) // 3
    if n <= 0:
        return ts, np.array([], dtype=np.int32)
    samples = np.empty(n, dtype=np.int32)
    for i in range(n):
        samples[i] = s24le_to_int(data[i*3:(i*3)+3])
    return ts, samples

# ---------------- Filters + SQI ----------------
def design_filters(fs: float):
    nyq = 0.5 * fs
    lo = BANDPASS_LOW_HZ / nyq
    hi = BANDPASS_HIGH_HZ / nyq
    b_bp, a_bp = butter(4, [lo, hi], btype="bandpass")
    w0 = NOTCH_HZ / nyq
    b_n, a_n = iirnotch(w0, NOTCH_Q)
    return (b_bp, a_bp), (b_n, a_n)

def apply_filters(x: np.ndarray, fs: float, cache: dict):
    if len(x) < int(fs * 2):
        return x.astype(np.float64)
    key = round(fs, 2)
    if key not in cache:
        cache[key] = design_filters(fs)
    (b_bp, a_bp), (b_n, a_n) = cache[key]
    y = x.astype(np.float64)
    y = filtfilt(b_n, a_n, y)
    y = filtfilt(b_bp, a_bp, y)
    return y

def ecg_sqi(raw: np.ndarray, fs: float) -> float:
    """
    Simple-but-strong SQI:
    - penalize flatlines / repeated samples
    - penalize low dynamic range
    - penalize extreme clipping
    - reward reasonable band power vs total power
    Returns 0..1
    """
    if len(raw) < int(fs * 2):
        return 0.0

    x = raw.astype(np.float64)
    # flat / identical
    identical = float(np.mean(np.diff(x) == 0.0))
    if identical > 0.25:
        return 0.1

    # dynamic range
    p2p = np.percentile(x, 99) - np.percentile(x, 1)
    if p2p < 200:  # depends on your scaling; keep conservative
        base = 0.15
    else:
        base = clamp(p2p / 8000.0, 0.15, 1.0)

    # clipping
    hi = np.percentile(x, 99.9)
    lo = np.percentile(x, 0.1)
    clip_frac = float(np.mean((x >= hi) | (x <= lo)))
    clip_pen = clamp(1.0 - 2.5 * clip_frac, 0.0, 1.0)

    # spectral sanity: band (0.5-40) vs total (0-65)
    f, Pxx = welch(x - np.mean(x), fs=fs, nperseg=min(len(x), int(fs*4)))
    total = np.trapz(Pxx[(f >= 0.1) & (f <= min(65.0, fs/2-1e-6))], f[(f >= 0.1) & (f <= min(65.0, fs/2-1e-6))]) + 1e-12
    band  = np.trapz(Pxx[(f >= 0.5) & (f <= min(40.0, fs/2-1e-6))], f[(f >= 0.5) & (f <= min(40.0, fs/2-1e-6))])
    band_ratio = clamp(float(band / total), 0.0, 1.0)

    sqi = base * clip_pen * (0.4 + 0.6 * band_ratio)
    return float(clamp(sqi, 0.0, 1.0))

# ---------------- QRS + RR ----------------
def qrs_detect(ecg_f: np.ndarray, fs: float) -> np.ndarray:
    if len(ecg_f) < int(fs * 2):
        return np.array([], dtype=int)

    d = np.diff(ecg_f, prepend=ecg_f[0])
    e = d * d
    win = max(3, int(0.150 * fs))
    mwi = np.convolve(e, np.ones(win) / win, mode="same")

    med = np.median(mwi)
    mad = np.median(np.abs(mwi - med)) + 1e-12
    prom = PEAK_PROM_FRAC * (mad * 10.0)

    dist = int(REFRACTORY_SEC * fs)
    peaks, _ = find_peaks(mwi, distance=dist, prominence=prom)

    refined = []
    search = int(0.08 * fs)
    for p in peaks:
        a = max(0, p - search)
        b = min(len(ecg_f), p + search + 1)
        refined.append(a + int(np.argmax(ecg_f[a:b])))
    return np.array(refined, dtype=int)

def rr_from_peaks(peaks: np.ndarray, fs: float) -> Optional[np.ndarray]:
    if len(peaks) < 3:
        return None
    rr = np.diff(peaks) / fs  # seconds
    # remove impossible RR
    rr = rr[(rr > 0.25) & (rr < 2.5)]
    if len(rr) < 3:
        return None
    return rr

def clean_rr(rr: np.ndarray) -> np.ndarray:
    """
    Light ectopy/outlier cleaning: remove RR far from median (winsor-ish).
    Keeps demo stable without hiding everything.
    """
    med = np.median(rr)
    dev = np.median(np.abs(rr - med)) + 1e-9
    lo = med - 4.0 * dev
    hi = med + 4.0 * dev
    rr2 = rr[(rr >= lo) & (rr <= hi)]
    return rr2 if len(rr2) >= 3 else rr

# ---------------- HRV metrics ----------------
def time_domain_hrv(rr: np.ndarray) -> dict:
    rr_ms = rr * 1000.0
    diff = np.diff(rr_ms)
    bpm = 60.0 / np.median(rr) if np.median(rr) > 0 else None

    sdnn = float(np.std(rr_ms, ddof=1)) if len(rr_ms) >= 2 else None
    rmssd = float(np.sqrt(np.mean(diff**2))) if len(diff) >= 2 else None
    pnn50 = float(np.mean(np.abs(diff) > 50.0) * 100.0) if len(diff) >= 2 else None
    pnn20 = float(np.mean(np.abs(diff) > 20.0) * 100.0) if len(diff) >= 2 else None

    mean_rr = float(np.mean(rr))
    cvrr = float(np.std(rr, ddof=1) / (mean_rr + 1e-9)) if len(rr) >= 2 else None

    # Poincaré
    if len(rr_ms) >= 3:
        rr1 = rr_ms[:-1]
        rr2 = rr_ms[1:]
        sd1 = float(np.sqrt(np.var((rr2 - rr1) / np.sqrt(2), ddof=1)))
        sd2 = float(np.sqrt(np.var((rr2 + rr1) / np.sqrt(2), ddof=1)))
    else:
        sd1, sd2 = None, None

    return {
        "bpm": float(bpm) if bpm is not None else None,
        "rr_ms_mean": float(np.mean(rr_ms)),
        "rr_ms_med": float(np.median(rr_ms)),
        "sdnn_ms": sdnn,
        "rmssd_ms": rmssd,
        "pnn50_pct": pnn50,
        "pnn20_pct": pnn20,
        "cvrr": cvrr,
        "sd1_ms": sd1,
        "sd2_ms": sd2,
        "n_rr": int(len(rr)),
    }

def interpolate_tachogram(rr: np.ndarray, fs_resample: float = 4.0):
    """
    For frequency-domain HRV:
    - build cumulative time of RR
    - interpolate instantaneous RR to uniform grid
    """
    t = np.cumsum(rr)
    t = t - t[0]
    if t[-1] < 10.0:
        return None, None
    tt = np.arange(0.0, t[-1], 1.0/fs_resample)
    rr_i = np.interp(tt, t, rr)
    rr_i = rr_i - np.mean(rr_i)
    return tt, rr_i

def freq_domain_hrv(rr: np.ndarray) -> dict:
    """
    LF: 0.04–0.15 Hz, HF: 0.15–0.40 Hz
    """
    tt, rr_i = interpolate_tachogram(rr, fs_resample=4.0)
    if tt is None:
        return {"lf_power": None, "hf_power": None, "lf_hf": None}

    f, Pxx = welch(rr_i, fs=4.0, nperseg=min(len(rr_i), 256))
    def bandpower(lo, hi):
        m = (f >= lo) & (f < hi)
        if not np.any(m):
            return None
        return float(np.trapz(Pxx[m], f[m]))

    lf = bandpower(0.04, 0.15)
    hf = bandpower(0.15, 0.40)
    if lf is None or hf is None or hf <= 1e-12:
        lf_hf = None
    else:
        lf_hf = float(lf / hf)
    return {"lf_power": lf, "hf_power": hf, "lf_hf": lf_hf}

def sample_entropy(x: np.ndarray, m: int = 2, r: float = 0.2) -> Optional[float]:
    """
    SampEn of RR (seconds). r is fraction of std.
    O(N^2) but RR per window is small => OK.
    """
    if len(x) < (m + 2):
        return None
    x = np.asarray(x, dtype=np.float64)
    sd = np.std(x, ddof=1)
    if sd < 1e-9:
        return 0.0
    tol = r * sd

    def _phi(mm):
        N = len(x)
        count = 0
        total = 0
        for i in range(N - mm):
            xi = x[i:i+mm]
            for j in range(i+1, N - mm + 1):
                xj = x[j:j+mm]
                if np.max(np.abs(xi - xj)) <= tol:
                    count += 1
            total += (N - mm - i)
        return count / (total + 1e-12)

    A = _phi(m+1)
    B = _phi(m)
    if A <= 1e-12 or B <= 1e-12:
        return None
    return float(-np.log(A / B))

def baevsky_stress_index(rr: np.ndarray) -> Optional[float]:
    """
    Baevsky Stress Index (SI) from RR histogram (ms).
    SI ~ AMo / (2 * Mo * MxDMn)
    AMo = mode amplitude (%)
    Mo = mode (s)
    MxDMn = range (s)
    """
    rr_ms = rr * 1000.0
    if len(rr_ms) < 10:
        return None
    # histogram binning (50 ms bins)
    bins = np.arange(np.min(rr_ms), np.max(rr_ms) + 50.0, 50.0)
    if len(bins) < 3:
        return None
    hist, edges = np.histogram(rr_ms, bins=bins)
    if np.sum(hist) == 0:
        return None
    i_mode = int(np.argmax(hist))
    mo_ms = 0.5 * (edges[i_mode] + edges[i_mode+1])
    amo = float(hist[i_mode] / np.sum(hist) * 100.0)
    mxdmn_s = float((np.max(rr_ms) - np.min(rr_ms)) / 1000.0) + 1e-9
    mo_s = float(mo_ms / 1000.0) + 1e-9
    si = amo / (2.0 * mo_s * mxdmn_s)
    return float(si)

# ---------------- Heat / strain indices ----------------
def core_temp_estimate_c(hr_bpm: float, baseline_hr: float, ambient_c: Optional[float] = None) -> float:
    """
    Demo heuristic: HR-driven core temperature estimate.
    Literature supports HR-derived core temperature estimation being viable in firefighter-like tasks
    (device algorithms differ; this is a simplified estimator for demo visuals). :contentReference[oaicite:2]{index=2}

    We keep it conservative and smooth; you can later replace with a validated model.
    """
    if hr_bpm is None:
        return 37.0
    amb = ambient_c if ambient_c is not None else 25.0
    # HR above baseline lifts estimated core temp
    dhr = max(0.0, hr_bpm - baseline_hr)
    # ambient adds mild thermal load
    tc = 36.8 + 0.018 * dhr + 0.02 * max(0.0, amb - 25.0)
    return float(clamp(tc, 36.5, 40.2))

def physiological_strain_index(hr: float, tc: float) -> Optional[float]:
    """
    Classic PSI style (0..10) using HR + core temp estimate.
    """
    if hr is None or tc is None:
        return None
    psi = 5.0 * (tc - 37.0) / (39.5 - 37.0) + 5.0 * (hr - 60.0) / (180.0 - 60.0)
    return float(clamp(psi, 0.0, 10.0))

# ---------------- Risk scoring (demo-friendly) ----------------
def fatigue_score(metrics: dict) -> Optional[float]:
    """
    0..100 composite:
    - high HR (relative) + low RMSSD + high LF/HF + high stress index + low entropy
    Great for dashboards: one number + drill-down metrics.
    """
    bpm = metrics.get("bpm")
    rmssd = metrics.get("rmssd_ms")
    lf_hf = metrics.get("lf_hf")
    si = metrics.get("stress_index")
    sampen = metrics.get("sampen")

    if bpm is None:
        return None

    s = 0.0
    # HR contribution (cap)
    s += clamp((bpm - 100.0) / 80.0, 0.0, 1.0) * 30.0
    # RMSSD inverse
    if rmssd is not None:
        s += clamp((25.0 - rmssd) / 25.0, 0.0, 1.0) * 30.0
    # LF/HF
    if lf_hf is not None:
        s += clamp((lf_hf - 1.5) / 2.5, 0.0, 1.0) * 15.0
    # Stress index
    if si is not None:
        s += clamp((si - 150.0) / 250.0, 0.0, 1.0) * 15.0
    # Entropy (low entropy can reflect constrained variability)
    if sampen is not None:
        s += clamp((1.2 - sampen) / 1.2, 0.0, 1.0) * 10.0

    return float(clamp(s, 0.0, 100.0))

def cardiac_risk_score(metrics: dict) -> Optional[float]:
    """
    0..100 *operational* cardiac concern score:
    - extreme HR
    - irregular rhythm proxy (CVRR)
    - ectopy ratio proxy (outlier RR fraction)
    - very low SQI prevents scoring
    """
    sqi = metrics.get("sqi", 0.0)
    if sqi is None or sqi < LOW_SQI:
        return None

    bpm = metrics.get("bpm")
    cvrr = metrics.get("cvrr")
    ect = metrics.get("ectopy_ratio")

    if bpm is None:
        return None

    s = 0.0
    # HR extremes
    if bpm > 175:
        s += 35.0
    elif bpm > 160:
        s += 20.0
    if bpm < 42:
        s += 35.0
    elif bpm < 50:
        s += 15.0

    # irregularity
    if cvrr is not None:
        s += clamp((cvrr - 0.10) / 0.15, 0.0, 1.0) * 30.0

    # ectopy
    if ect is not None:
        s += clamp(ect / 0.10, 0.0, 1.0) * 35.0

    return float(clamp(s, 0.0, 100.0))

# ---------------- State ----------------
@dataclass
class StreamState:
    fs_est: float = DEFAULT_FS
    buf: deque = field(default_factory=lambda: deque(maxlen=int(DEFAULT_FS * WINDOW_SEC * 4)))
    last_ts: Optional[int] = None
    last_n: Optional[int] = None

    # baseline tracking (for relative indices)
    baseline_hr: float = 85.0
    baseline_rmssd: float = 30.0
    baseline_tc: float = 37.0

    # heat strain AUC accumulator (relative to start of shift)
    auc_start_iso: Optional[str] = None
    auc_ref_tc: Optional[float] = None
    auc_area: float = 0.0
    auc_last_tc: Optional[float] = None
    auc_last_t: Optional[float] = None

states: Dict[Tuple[str, str], StreamState] = {}
filt_cache: dict = {}

def update_baselines(st: StreamState, bpm: float, rmssd: Optional[float], tc: float):
    # slow EMA so baselines are stable
    st.baseline_hr = 0.98 * st.baseline_hr + 0.02 * bpm
    if rmssd is not None:
        st.baseline_rmssd = 0.98 * st.baseline_rmssd + 0.02 * rmssd
    st.baseline_tc = 0.995 * st.baseline_tc + 0.005 * tc

def update_heat_auc(st: StreamState, tc: float, t_now: float):
    """
    AUC over (tc - ref) vs time using trapezoids.
    Demo-friendly cumulative heat load, inspired by AUC idea in firefighter heat strain work. :contentReference[oaicite:3]{index=3}
    """
    if st.auc_start_iso is None:
        st.auc_start_iso = now_iso()
        st.auc_ref_tc = tc
        st.auc_last_tc = tc
        st.auc_last_t = t_now
        st.auc_area = 0.0
        return

    if st.auc_last_t is None or st.auc_last_tc is None or st.auc_ref_tc is None:
        return

    dt = t_now - st.auc_last_t
    if dt <= 0.0 or dt > 60.0:
        # ignore huge gaps
        st.auc_last_t = t_now
        st.auc_last_tc = tc
        return

    y0 = st.auc_last_tc - st.auc_ref_tc
    y1 = tc - st.auc_ref_tc
    st.auc_area += 0.5 * (y0 + y1) * dt  # degC * sec
    st.auc_last_t = t_now
    st.auc_last_tc = tc

# ---------------- Publishing ----------------
def publish(client: mqtt.Client, team: str, ff: str, payload: dict, alerts: List[dict]):
    client.publish(PUB_TELEM_FMT.format(team=team, ff=ff), json.dumps(payload), qos=0)
    if alerts:
        client.publish(PUB_ALERT_FMT.format(team=team, ff=ff), json.dumps({
            "teamId": team,
            "ffId": ff,
            "observedAt": payload.get("observedAt", now_iso()),
            "alerts": alerts
        }), qos=0)

# ---------------- MQTT callbacks ----------------
def on_connect(client, userdata, flags, rc, properties=None):
    logging.info("Connected rc=%s, subscribing to %s", rc, SUB_TOPIC)
    client.subscribe(SUB_TOPIC, qos=0)

def on_message(client, userdata, msg):
    tf = topic_to_team_ff(msg.topic)
    if not tf:
        return
    team, ff = tf

    pkts = parse_ecg1_bundle(msg.payload)
    if not pkts:
        return

    key = (team, ff)
    if key not in states:
        states[key] = StreamState(
            fs_est=DEFAULT_FS,
            buf=deque(maxlen=int(WINDOW_SEC * DEFAULT_FS * 5))
        )
    st = states[key]

    # ingest packets
    for p in pkts:
        ts, samples = parse_polar_pmd_ecg_packet(p)
        if samples is None or len(samples) == 0:
            continue

        # fs estimate from timestamps (ns)
        if st.last_ts is not None and ts is not None and ts > st.last_ts and st.last_n:
            dt = ts - st.last_ts
            if dt > 1e6:
                dt_sec = dt / 1e9
                if 0.001 < dt_sec < 2.0:
                    fs_new = float(st.last_n) / dt_sec
                    if 50.0 < fs_new < 300.0:
                        st.fs_est = 0.9 * st.fs_est + 0.1 * fs_new

        st.last_ts = ts
        st.last_n = len(samples)

        for v in samples:
            st.buf.append(int(v))

    fs = float(st.fs_est)
    need = int(MIN_SEC_FOR_ANALYSIS * fs)
    if len(st.buf) < need:
        return

    n = min(int(WINDOW_SEC * fs), len(st.buf))
    raw = np.array(list(st.buf)[-n:], dtype=np.float64)

    # SQI + filter + QRS
    sqi = ecg_sqi(raw, fs)
    flt = apply_filters(raw, fs, filt_cache)
    peaks = qrs_detect(flt, fs)
    rr = rr_from_peaks(peaks, fs)
    if rr is None:
        return

    rr = clean_rr(rr)

    # HRV metrics
    td = time_domain_hrv(rr)
    fd = freq_domain_hrv(rr)
    sampen = sample_entropy(rr, m=2, r=0.2)
    si = baevsky_stress_index(rr)

    # ectopy ratio proxy: fraction of RR removed by clean_rr (approx)
    # (we can estimate as how many are far from median)
    med = np.median(rr)
    dev = np.median(np.abs(rr - med)) + 1e-9
    ect = float(np.mean((rr < med - 3*dev) | (rr > med + 3*dev)))

    bpm = td.get("bpm")
    if bpm is None:
        return

    # heat / strain
    tc_est = core_temp_estimate_c(bpm, st.baseline_hr, ambient_c=None)
    psi = physiological_strain_index(bpm, tc_est)

    # update baselines slowly (only if quality OK)
    if sqi >= 0.35:
        update_baselines(st, bpm, td.get("rmssd_ms"), tc_est)

    # heat AUC update
    update_heat_auc(st, tc_est, time.time())

    # Composite scores
    metrics = {
        "teamId": team,
        "ffId": ff,
        "observedAt": now_iso(),
        "source": "biomed_agent_strong",
        "fs_est_hz": round(fs, 2),
        "sqi": float(sqi),

        # Time-domain HRV
        **td,

        # Freq-domain
        **fd,

        # Nonlinear / stress
        "sampen": sampen,
        "stress_index": si,

        # rhythm proxies
        "ectopy_ratio": ect,

        # heat/strain
        "tc_est_c": tc_est,
        "psi": psi,
        "heat_auc_degC_sec": st.auc_area,
        "heat_auc_since": st.auc_start_iso,
        "baseline_hr_bpm": round(st.baseline_hr, 1),
        "baseline_rmssd_ms": round(st.baseline_rmssd, 1),
        "baseline_tc_c": round(st.baseline_tc, 2),
    }

    # scores
    metrics["fatigue_score_0_100"] = fatigue_score({
        **metrics,
        "lf_hf": fd.get("lf_hf"),
        "rmssd_ms": td.get("rmssd_ms"),
    })
    metrics["cardiac_risk_score_0_100"] = cardiac_risk_score(metrics)

    # Alerts
    alerts = []
    if sqi < LOW_SQI:
        alerts.append({"type": "signal_quality", "severity": "info", "msg": f"Low ECG SQI ({sqi:.2f})"})

    if sqi >= LOW_SQI:
        if bpm > TACHY_BPM:
            alerts.append({"type": "tachy", "severity": "danger", "msg": f"High HR {bpm:.0f} bpm"})
        if bpm < BRADY_BPM:
            alerts.append({"type": "brady", "severity": "danger", "msg": f"Low HR {bpm:.0f} bpm"})

        rmssd = td.get("rmssd_ms")
        if rmssd is not None and rmssd < LOW_RMSSD_MS:
            alerts.append({"type": "fatigue_hrv", "severity": "warn",
                           "msg": f"Low RMSSD {rmssd:.1f} ms (fatigue/stress proxy)"})

        lf_hf = fd.get("lf_hf")
        if lf_hf is not None and lf_hf > HIGH_LFHF:
            alerts.append({"type": "sympathetic_load", "severity": "warn",
                           "msg": f"High LF/HF {lf_hf:.2f} (sympathetic dominance proxy)"})

        cvrr = td.get("cvrr")
        if cvrr is not None and cvrr > IRREGULAR_WARN:
            alerts.append({"type": "irregular_rhythm", "severity": "warn",
                           "msg": f"Irregular RR variability (CVRR {cvrr:.2f})"})

        if psi is not None:
            if psi >= PSI_DANGER:
                alerts.append({"type": "heat_strain", "severity": "danger",
                               "msg": f"High physiological strain (PSI {psi:.1f}, Tc_est {tc_est:.2f}°C)"})
            elif psi >= PSI_WARN:
                alerts.append({"type": "heat_strain", "severity": "warn",
                               "msg": f"Rising physiological strain (PSI {psi:.1f}, Tc_est {tc_est:.2f}°C)"})

        fscr = metrics.get("fatigue_score_0_100")
        if fscr is not None and fscr >= 70:
            alerts.append({"type": "fatigue_composite", "severity": "warn",
                           "msg": f"High fatigue score {fscr:.0f}/100"})

        cr = metrics.get("cardiac_risk_score_0_100")
        if cr is not None and cr >= 70:
            alerts.append({"type": "cardiac_concern", "severity": "danger",
                           "msg": f"Elevated cardiac concern {cr:.0f}/100 (operational flag)"})

    publish(client, team, ff, metrics, alerts)

def main():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_forever()

if __name__ == "__main__":
    main()
