#!/usr/bin/env python3
"""Frame-pacing analysis of a Frame Generation present trace (Threshold: `quiver lab call <i> fg trace`,
src/features/fg/trace.cpp; ported from Pharos tools/fg-pacing.py): <dir>/presents.csv + <dir>/bands.bin.

Content: horizontal image shift between consecutive presented frames (phase correlation on two
sub-bands left/right of the player character) during a constant-rate camera yaw. For each
(real N-1, generated N, real N) triplet the generated frame should sit at ratio ~0.5 of the
real-to-real shift. Timing: present-call intervals split by transition (G->R, R->G), Present
call durations, game arrival intervals. Smoothness: cumulative image position vs present time
(a constant-rate spin should be a straight line); residual in px and in ms.

usage: fg-pacing.py <dir> [--json out.json]
"""
import json
import struct
import sys

import numpy as np


def load(d):
    rows, rgb = load_rgb(d)
    if rgb is None:
        return rows, None
    return rows, rgb @ np.array([0.299, 0.587, 0.114], dtype=np.float32)


def load_rgb(d):
    rows = []
    with open(f"{d}/presents.csv") as f:
        hdr = f.readline().strip().split(",")
        for line in f:
            v = line.strip().split(",")
            if len(v) != len(hdr):
                continue
            r = dict(zip(hdr, v))
            for k in hdr:
                if k != "kind":
                    r[k] = int(r[k])
            # Multi Frame Generation traces: gi = generated index (1..gn-1), gn = multiplier. Older
            # traces (2x only) don't have them.
            r.setdefault("gi", 1 if r["kind"] == "G" else 0)
            r.setdefault("gn", 2)
            rows.append(r)
    bands = None
    try:
        with open(f"{d}/bands.bin", "rb") as f:
            magic, w, h, n, fmt = struct.unpack("<5I", f.read(20))
            raw = f.read()
            n = min(n, len(raw) // (w * h * 4))
            if fmt == 2:  # R10G10B10A2_UNORM (Threshold: Control's SDR swapchain)
                px = np.frombuffer(raw, dtype="<u4")[: n * w * h].reshape(n, h, w)
                rgb = np.stack([(px >> s) & 0x3FF for s in (0, 10, 20)], axis=-1).astype(np.float32) * (255.0 / 1023.0)
            else:  # 8-bit RGBA (0) / BGRA (1)
                data = np.frombuffer(raw, dtype=np.uint8)
                b8 = data[: n * w * h * 4].reshape(n, h, w, 4).astype(np.float32)
                rgb = b8[..., [2, 1, 0]] if fmt == 1 else b8[..., :3]
            bands = rgb
    except FileNotFoundError:
        pass
    return rows, bands


def phase_shift(a, b):
    """Shift (dx, dy) such that b(x) ~ a(x - dx), sub-pixel, plus peak strength."""
    h, w = a.shape
    win = np.outer(np.hanning(h), np.hanning(w)).astype(np.float32)
    A = np.fft.fft2((a - a.mean()) * win)
    B = np.fft.fft2((b - b.mean()) * win)
    R = B * np.conj(A)
    R /= np.abs(R) + 1e-6
    c = np.real(np.fft.ifft2(R))
    iy, ix = np.unravel_index(np.argmax(c), c.shape)
    peak = c[iy, ix]

    def sub(cm, c0, cp):
        d = cm - 2 * c0 + cp
        return 0.0 if abs(d) < 1e-12 else 0.5 * (cm - cp) / d

    dx = ix + sub(c[iy, (ix - 1) % w], peak, c[iy, (ix + 1) % w])
    dy = iy + sub(c[(iy - 1) % h, ix], peak, c[(iy + 1) % h, ix])
    if dx > w / 2:
        dx -= w
    if dy > h / 2:
        dy -= h
    return dx, dy, peak


def shifts(bands, i, j):
    w = bands.shape[2]
    out = []
    for x0, x1 in ((int(0.04 * w), int(0.36 * w)), (int(0.64 * w), int(0.96 * w))):
        dx, dy, pk = phase_shift(bands[i][:, x0:x1], bands[j][:, x0:x1])
        out.append((dx, pk))
    return out


def stats(v):
    v = np.asarray(v, dtype=float)
    if v.size == 0:
        return {"n": 0}
    return {"n": int(v.size), "mean": float(v.mean()), "std": float(v.std()), "p1": float(np.percentile(v, 1)),
            "p10": float(np.percentile(v, 10)), "p50": float(np.percentile(v, 50)),
            "p90": float(np.percentile(v, 90)), "p99": float(np.percentile(v, 99)), "min": float(v.min()),
            "max": float(v.max())}


def fmt(name, s, unit="ms"):
    if not s.get("n"):
        return f"  {name}: none"
    return (f"  {name}: n={s['n']} mean {s['mean']:.3f} sd {s['std']:.3f} | p1 {s['p1']:.3f} p10 {s['p10']:.3f} "
            f"p50 {s['p50']:.3f} p90 {s['p90']:.3f} p99 {s['p99']:.3f} | min {s['min']:.3f} max {s['max']:.3f} {unit}")


def local_residual(tt_all, pp_all):
    out = []
    for i in range(4, len(tt_all) - 4):
        tt, pp = tt_all[i - 4:i + 5], pp_all[i - 4:i + 5]
        c, *_ = np.linalg.lstsq(np.vstack([tt, np.ones_like(tt)]).T, pp, rcond=None)
        out.append(pp_all[i] - (c[0] * tt_all[i] + c[1]))
    return out


def display_models(t, pos, res):
    """Fixed-refresh display models (no VRR): which frame each vblank shows. mailbox: the newest frame
    presented before the vblank (compositor latch 1 ms before); fifo: every frame in order at the first
    free vblank. Averaged over 8 vblank phases. Reports vblanks repeating the previous frame, frames
    never shown, and |displayed position - local linear fit| (judder as seen on screen); vrr shows
    each frame at its present time."""
    hz = 60.0
    for a in sys.argv:
        if a.startswith("--hz="):
            hz = float(a[5:])
    per = 1000.0 / hz
    for model in ("vrr", "mailbox", "fifo"):
        reps = drops = n_vb = 0
        resid = []
        for ph in np.linspace(0, per, 8, endpoint=False):
            if model == "vrr":
                resid += local_residual(t, pos)
                break
            vb = np.arange(t[0] + ph, t[-1], per)
            if model == "mailbox":
                idx = np.searchsorted(t + 1.0, vb, side="right") - 1
            else:
                idx = np.full(len(vb), -1)
                last = -1
                for i in range(len(t)):
                    k = max(int(np.searchsorted(vb, t[i] + 1.0)), last + 1)
                    last = k
                    if k < len(vb):
                        idx[k:] = i
            ok = idx >= 0
            vb, idx = vb[ok], idx[ok]
            reps += int((np.diff(idx) == 0).sum())
            drops += int(len(t) - len(np.unique(idx)))
            n_vb += len(vb)
            resid += local_residual(vb, pos[idx])
        r = stats(np.abs(resid))
        res[f"display_{model}"] = {"resid_abs_px": r, "repeat_vblanks": reps, "dropped_frames": drops, "vblanks": n_vb}
        extra = "" if model == "vrr" else (f"; vblanks repeating a frame {100.0 * reps / max(n_vb, 1):.1f}%, frames "
                                           f"never shown {100.0 * drops / (8 * len(t)):.1f}%")
        print(f"  display {model:7s} @{hz:.0f} Hz: |position residual| mean {r['mean']:.2f} p90 {r['p90']:.2f} "
              f"max {r['max']:.2f} px{extra}")


def main():
    d = sys.argv[1]
    rows, bands = load(d)
    res = {}
    kinds = "".join(r["kind"] for r in rows)
    print(f"{d}: {len(rows)} presents ({kinds.count('G')} G, {kinds.count('R')} R, {kinds.count('N')} N)")
    span = (rows[-1]["call_us"] - rows[0]["call_us"]) / 1e6 if len(rows) > 1 else 0
    if span:
        print(f"  span {span:.2f} s: {len(rows) / span:.1f} presents/s, real/normal "
              f"{(kinds.count('R') + kinds.count('N')) / span:.1f}/s")

    # ---- timing
    trans = {}
    for a, b in zip(rows, rows[1:]):
        trans.setdefault(a["kind"] + b["kind"], []).append((b["call_us"] - a["call_us"]) / 1000)
    for k in sorted(trans):
        res[f"interval_{k}"] = stats(trans[k])
        print(fmt(f"present-call interval {k[0]}->{k[1]}", res[f"interval_{k}"]))
    allint = [(b["call_us"] - a["call_us"]) / 1000 for a, b in zip(rows, rows[1:])]
    res["interval_all"] = stats(allint)
    print(fmt("present-call interval (all)", res["interval_all"]))
    for k in "GRN":
        dur = [(r["ret_us"] - r["call_us"]) / 1000 for r in rows if r["kind"] == k and r["ret_us"]]
        if dur:
            res[f"present_dur_{k}"] = stats(dur)
            print(fmt(f"Present() duration {k}", res[f"present_dur_{k}"]))
    real = [r for r in rows if r["kind"] in "RN"]
    arr = sorted({r["arrive_us"] for r in real})
    res["arrival_interval"] = stats(np.diff(arr) / 1000)
    print(fmt("game Present arrival interval", res["arrival_interval"]))
    for k in "GR":
        lat = [(r["call_us"] - r["arrive_us"]) / 1000 for r in rows if r["kind"] == k]
        if lat:
            res[f"after_arrival_{k}"] = stats(lat)
            print(fmt(f"{k} present after game arrival", res[f"after_arrival_{k}"]))
    # Order: every G must be presented after the previous R and before its own R; with Multi Frame
    # Generation the generated frames of one real frame in index order (gi = 1, 2, ...).
    inv = 0
    for a, b in zip(rows, rows[1:]):
        if a["kind"] == "G" and b["kind"] == "R" and a["frame"] != b["frame"]:
            inv += 1
        if a["kind"] == "G" and b["kind"] == "G" and (a["frame"] != b["frame"] or b["gi"] != a["gi"] + 1):
            inv += 1
        if b["call_us"] < a["call_us"]:
            inv += 1
    res["order_violations"] = inv
    print(f"  ordering violations (timestamps / G-R pairing / index order): {inv}")
    mults = sorted({r["gn"] for r in rows if r["kind"] in "GR"})
    if mults:
        cnt = {m: sum(1 for r in rows if r["kind"] == "R" and r["gn"] == m) for m in mults}
        res["multipliers"] = {str(m): c for m, c in cnt.items()}
        print("  multipliers (real frames): " + ", ".join(f"{m}x {c}" for m, c in cnt.items()))
    # Display-time error vs content time: content of real N = its arrival (game cadence), generated
    # frame k of N (multiplier m) = k/m of the way from the previous arrival to this one. (call -
    # content) should be constant.
    last = None
    errs = []
    for r in rows:
        if r["kind"] in "RN":
            last = r["arrive_us"]
            errs.append((r["call_us"] - r["arrive_us"]) / 1000)
        elif r["kind"] == "G" and last is not None:
            errs.append((r["call_us"] - (last + (r["arrive_us"] - last) * r["gi"] / max(r["gn"], 2))) / 1000)
    if errs:
        e = np.array(errs)
        res["present_minus_content"] = stats(e - np.median(e))
        print(fmt("present time - content time (arrival model), minus median", res["present_minus_content"]))

    if bands is None or len(bands) < 3:
        print(json.dumps(res) if "--json" in sys.argv else "", end="")
        return
    # ---- content
    shot_rows = [r for r in rows if r["shot"] >= 0]
    shot_rows.sort(key=lambda r: r["shot"])
    sh = []  # per consecutive pair: (dx_left, dx_right, peak_l, peak_r)
    for a, b in zip(shot_rows, shot_rows[1:]):
        (l, pl), (r_, pr) = shifts(bands, a["shot"], b["shot"])
        sh.append((l, r_, pl, pr))
    # Runs R(N-1) G1(N) .. Gk(N) R(N): generated frame i should sit at i/m of the real-to-real shift
    # (2x: 0.5). d(R, Gi) is the sum of the consecutive shifts up to Gi.
    ratios, agree = [], []
    per_index = {}  # (i, m) -> ratios
    nonmono = runs = 0
    for i in range(len(shot_rows) - 2):
        a = shot_rows[i]
        if a["kind"] != "R" or shot_rows[i + 1]["kind"] != "G":
            continue
        j = i + 1
        while j < len(shot_rows) and shot_rows[j]["kind"] == "G":
            j += 1
        if j >= len(shot_rows):
            continue
        b = shot_rows[j]
        gens = shot_rows[i + 1:j]
        if b["kind"] != "R" or any(g["frame"] != b["frame"] for g in gens):
            continue
        rr = shifts(bands, a["shot"], b["shot"])
        runs += 1
        for side in (0, 1):
            # 2x: the direct R->R' shift (as before). Multi Frame Generation: the sum of the run's
            # consecutive shifts - at 3x+ one real frame moves the image by 100-200 px in a spin, more
            # than the band's phase correlation measures reliably in one step.
            dt = rr[side][0] if len(gens) == 1 else sum(sh[m][side] for m in range(i, j))
            if abs(dt) < 2.0 or min(sh[m][2 + side] for m in range(i, j)) < 0.05:
                continue
            cum, prev_ratio = 0.0, -1e9
            for m, g in enumerate(gens):
                cum += sh[i + m][side]
                q = cum / dt
                key = (g["gi"], g["gn"])
                per_index.setdefault(key, []).append(q)
                ratios.append(q)
                if q < prev_ratio:
                    nonmono += 1
                prev_ratio = q
            agree.append(abs(cum + sh[j - 1][side] - dt))
    if ratios:
        res["gen_ratio"] = stats(ratios)
        res["gen_ratio_closure_px"] = stats(agree)
        two_x = all(k[1] == 2 for k in per_index)
        print(fmt("generated-frame shift ratio (0.5 = midpoint)" if two_x else
                  "generated-frame shift ratio, all indices", res["gen_ratio"], ""))
        print(fmt("  closure |sum d(R..G..R') - d(R,R')|", res["gen_ratio_closure_px"], "px"))
        if not two_x:
            res["gen_ratio_index"] = {}
            for (gi, gn) in sorted(per_index, key=lambda k: (k[1], k[0])):
                s = stats(per_index[(gi, gn)])
                res["gen_ratio_index"][f"{gi}/{gn}"] = s
                print(fmt(f"  shift ratio index {gi} of {gn}x (expected {gi / gn:.3f})", s, ""))
        r = np.array(ratios)
        res["gen_ratio_inverted"] = int(((r < 0) | (r > 1)).sum())
        res["gen_ratio_nonmonotonic"] = nonmono
        print(f"  ratios outside [0,1] (inverted/overshoot): {res['gen_ratio_inverted']} of {len(r)}; "
              f"out of order within a run: {nonmono} ({runs} runs)")
    # Per-step shift by transition, and the smoothness of position vs present time.
    steps = {}
    for (a, b), s in zip(zip(shot_rows, shot_rows[1:]), sh):
        steps.setdefault(a["kind"] + b["kind"], []).append(0.5 * (s[0] + s[1]))
    for k in sorted(steps):
        res[f"step_px_{k}"] = stats(steps[k])
        print(fmt(f"image shift per present {k[0]}->{k[1]}", res[f"step_px_{k}"], "px"))
    pos = np.concatenate([[0.0], np.cumsum([0.5 * (s[0] + s[1]) for s in sh])])
    t = np.array([r["call_us"] for r in shot_rows], dtype=float) / 1000
    t -= t[0]  # (least squares on absolute QPC ms is ill-conditioned)
    if len(t) > 4 and np.ptp(pos) > 10:
        A = np.vstack([t, np.ones_like(t)]).T
        coef, *_ = np.linalg.lstsq(A, pos, rcond=None)
        # Detrend over a short window (the camera's own speed varies slowly): residual of a
        # moving linear fit over 9 presents.
        resid = []
        for i in range(4, len(t) - 4):
            tt, pp = t[i - 4:i + 5], pos[i - 4:i + 5]
            c, *_ = np.linalg.lstsq(np.vstack([tt, np.ones_like(tt)]).T, pp, rcond=None)
            resid.append(pos[i] - (c[0] * t[i] + c[1]))
        speed = coef[0]
        res["speed_px_per_ms"] = float(speed)
        res["position_residual_px"] = stats(resid)
        res["position_residual_ms"] = stats(np.array(resid) / speed)
        print(f"  spin speed {speed:.2f} px/ms ({speed * 1000:.0f} px/s)")
        print(fmt("position residual vs present time (local fit)", res["position_residual_px"], "px"))
        print(fmt("  same, as time error", res["position_residual_ms"], "ms"))
        v = [s / ((b["call_us"] - a["call_us"]) / 1000) for (a, b), s in
             zip(zip(shot_rows, shot_rows[1:]), [0.5 * (x[0] + x[1]) for x in sh]) if b["call_us"] > a["call_us"]]
        res["step_velocity_rel"] = stats(np.array(v) / speed)
        print(fmt("per-step apparent velocity / mean", res["step_velocity_rel"], ""))
        display_models(t, pos, res)
    if "--json" in sys.argv:
        with open(sys.argv[sys.argv.index("--json") + 1], "w") as f:
            json.dump(res, f, indent=1)


if __name__ == "__main__":
    main()
