import os
import json
import numpy as np
import wfdb

TEAM_ID = "Team_A"
FF_ID   = "FF_C"

RECORD = "418"
PN_DIR = "vfdb"

OUT_DIR = "out"
SEGMENT_SECONDS = 120
PRE_ROLL_SECONDS = 10

DOWNSAMPLE_TO_HZ = 130

OUT_HEADER = os.path.join(OUT_DIR, "ecg_segment_ff_c.h")
OUT_META   = os.path.join(OUT_DIR, "ecg_segment_ff_c.meta.json")
OUT_CREDITS = os.path.join(OUT_DIR, "CREDITS.md")
OUT_NOTICE  = os.path.join(OUT_DIR, "NOTICE.md")

def safe_mkdir(p):
    os.makedirs(p, exist_ok=True)

def find_episode_start_sample(record_name: str):
    """
    VFDB includes rhythm annotations via WFDB annotations.
    Not all records expose the same annotation names; we try common ones.
    If none are found, fall back to a strong-morphology window mid-record.
    """
    ann_candidates = ["atr", "ary", "ecg", "qrs", "vf", "vx"]
    for an in ann_candidates:
        try:
            ann = wfdb.rdann(record_name, an, pn_dir=PN_DIR)
            if len(ann.sample) > 0:
                return int(ann.sample[len(ann.sample)//2]), an
        except Exception:
            pass
    return None, None

def extract_segment():
    safe_mkdir(OUT_DIR)

    rec = wfdb.rdrecord(RECORD, pn_dir=PN_DIR)
    fs = float(rec.fs)

    sig = rec.p_signal
    if sig is None:
        raise RuntimeError("Record has no p_signal.")
    if sig.ndim != 2:
        raise RuntimeError("Unexpected signal dims.")

    ch = 0
    x = sig[:, ch].astype(np.float32)

    ep_sample, ann_name = find_episode_start_sample(RECORD)

    if ep_sample is None:
        ep_sample = int(0.5 * len(x))

    start = max(0, ep_sample - int(PRE_ROLL_SECONDS * fs))
    n = int(SEGMENT_SECONDS * fs)
    end = min(len(x), start + n)
    seg = x[start:end]

    # If we hit the end, pad by wrapping last value (keeps length exact)
    if len(seg) < n:
        pad = np.full((n - len(seg),), seg[-1] if len(seg) else 0.0, dtype=np.float32)
        seg = np.concatenate([seg, pad], axis=0)

    # Remove baseline wander with a simple high-pass via moving average subtraction
    win = int(fs * 0.8)
    if win < 5:
        win = 5
    kernel = np.ones(win, dtype=np.float32) / float(win)
    baseline = np.convolve(seg, kernel, mode="same")
    seg = seg - baseline

    # Downsample to target Hz
    target_fs = float(DOWNSAMPLE_TO_HZ)
    if target_fs > fs:
        target_fs = fs

    step = int(round(fs / target_fs))
    if step < 1:
        step = 1
    seg_ds = seg[::step]
    actual_fs = fs / step

    # Normalize amplitude robustly using percentile
    p95 = np.percentile(np.abs(seg_ds), 95)
    if p95 < 1e-6:
        p95 = 1e-6
    seg_norm = seg_ds / p95

    # Scale to int16 with headroom
    seg_i16 = np.clip(seg_norm * 1200.0, -1800.0, 1800.0).astype(np.int16)

    meta = {
        "teamId": TEAM_ID,
        "ffId": FF_ID,
        "source": {
            "provider": "PhysioNet",
            "dataset": "MIT-BIH Malignant Ventricular Ectopy Database (VFDB)",
            "record": RECORD,
            "pn_dir": PN_DIR,
            "annotation_used": ann_name,
            "segment_seconds": SEGMENT_SECONDS,
            "pre_roll_seconds": PRE_ROLL_SECONDS,
            "original_fs_hz": fs,
            "downsample_step": step,
            "output_fs_hz": actual_fs,
            "channel_index": 0
        }
    }

    # Write header
    with open(OUT_HEADER, "w", encoding="utf-8") as f:
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"static const uint16_t ECG_SEGMENT_FS_HZ = {int(round(actual_fs))};\n")
        f.write(f"static const uint32_t ECG_SEGMENT_LEN = {len(seg_i16)};\n")
        f.write(f"static const int16_t ECG_SEGMENT_DATA[{len(seg_i16)}] = {{\n")
        for i in range(0, len(seg_i16), 16):
            chunk = ", ".join(str(int(v)) for v in seg_i16[i:i+16])
            f.write("  " + chunk + ",\n")
        f.write("};\n")

    with open(OUT_META, "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2)

    # Credits / notice for GitHub
    with open(OUT_CREDITS, "w", encoding="utf-8") as f:
        f.write("# Credits\n\n")
        f.write("This project replays a short ECG segment derived from PhysioNet data for demonstration/testing.\n\n")
        f.write("Source dataset:\n")
        f.write("- MIT-BIH Malignant Ventricular Ectopy Database (VFDB), PhysioNet.\n")
        f.write(f"- Record: {RECORD}\n\n")
        f.write("License:\n")
        f.write("- PhysioNet data is commonly distributed under Open Data Commons Attribution (ODC-BY 1.0).\n")
        f.write("- You must attribute PhysioNet and the dataset when redistributing derived data.\n\n")
        f.write("Please cite the dataset page and any associated publication listed by PhysioNet.\n")

    with open(OUT_NOTICE, "w", encoding="utf-8") as f:
        f.write("NOTICE\n\n")
        f.write("This repository contains a derived ECG segment generated from PhysioNet VFDB data.\n")
        f.write("The segment is transformed (baseline removal, downsampling, scaling) for embedded replay.\n")
        f.write("Original data source: PhysioNet (VFDB). See CREDITS.md for attribution details.\n")

    print("Wrote:", OUT_HEADER)
    print("Wrote:", OUT_META)
    print("Wrote:", OUT_CREDITS)
    print("Wrote:", OUT_NOTICE)
    print("Done.")

if __name__ == "__main__":
    extract_segment()
