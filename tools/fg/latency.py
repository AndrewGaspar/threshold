#!/usr/bin/env python3
"""Input-to-frame latency for Threshold's frame generation (after Pharos tools/latency.py).

usage: latency.py <latency.csv> <inject.csv> [threshold_deg]

latency.csv: the mod's per-frame log (`quiver lab call <i> fg latency start|stop`, src/features/fg/latency.cpp),
CLOCK_MONOTONIC_RAW ns. inject.csv: `quiver lab mouse ... --repeat N --log FILE` (one row per send, a
CLOCK_MONOTONIC_RAW time column).

For every injected mouse motion at time t, the reaction frame is the first frame whose DLSS
evaluation was recorded after t with a camera turned by more than the threshold (threshold_deg, at
least 3x the 90th percentile of the frame-to-frame turn, so an idle sway doesn't count) from the
camera of the last frame recorded before t. Stages (ms after the input):
  eval recorded (CPU)  the game's DLSS SR evaluation recorded on the renderer thread
  DLSS ran (GPU)       GPU timestamp right after it
  frame done (GPU)     the queue reached our Present-time work: the frame's own GPU work is done
  DLSS-G done (GPU)    after the generated frames (FG on)
  first present        our first Present call carrying this frame's content (a generated frame
                       interpolated towards it, with FG; the real frame without)
  real present         the Present call of the real frame
Display scan-out / compositor time is not included (headless).
"""
import csv
import math
import statistics
import sys


def read(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def inject_times(path):
    rows = read(path)
    if not rows:
        return []
    keys = list(rows[0].keys())
    col = next((k for k in keys if "raw" in k.lower() and "ns" in k.lower()), None) or \
        next((k for k in keys if k.lower().endswith("_ns") or k.lower() == "ns"), None) or \
        next((k for k in keys if "time" in k.lower() or k.lower().startswith("t")), keys[0])
    v = [float(r[col]) for r in rows if r.get(col)]
    # seconds or microseconds -> ns
    if v and v[0] < 1e7:
        v = [x * 1e9 for x in v]
    elif v and v[0] < 1e13:
        v = [x * 1e3 for x in v]
    return [int(x) for x in v]


def angle(a, b):
    # rotation between two orthonormal bases (rows right, up, forward): acos((tr(A^T B) - 1) / 2)
    tr = sum(a[i] * b[i] for i in range(9))
    return math.degrees(math.acos(max(-1.0, min(1.0, (tr - 1) / 2))))


def main():
    lat, inj = sys.argv[1], sys.argv[2]
    thr = float(sys.argv[3]) if len(sys.argv) > 3 else 0.05
    fr = []
    for r in read(lat):
        if int(r["cpu_eval_ns"]) == 0:
            continue
        b = [float(r[k]) for k in ("r00", "r01", "r02", "r10", "r11", "r12", "r20", "r21", "r22")]
        fr.append((int(r["cpu_eval_ns"]), r, b))
    fr.sort(key=lambda x: x[0])
    ts = inject_times(inj)
    if len(fr) > 1:
        span = (fr[-1][0] - fr[0][0]) / 1e9
        gen = sum(int(x[1]["n_gen"]) for x in fr) / len(fr)
        print(f"frames: {len(fr)} over {span:.1f} s = {len(fr) / span:.1f} rendered fps, {gen:.2f} generated per frame")
    d = sorted(angle(fr[i - 1][2], fr[i][2]) for i in range(1, len(fr)))
    if d:
        # The camera can sway on its own (idle animation): keep the threshold well above that noise.
        p90 = d[int(len(d) * .9)]
        thr = max(thr, 3 * p90)
        print(f"camera frame-to-frame turn: median {d[len(d) // 2]:.4f} deg, p90 {p90:.4f}; threshold {thr:.3f}")
    stages = {k: [] for k in ("eval", "gpu_eval", "frame", "fg", "first", "real")}
    misses = 0
    for t in ts:
        before = [x for x in fr if x[0] < t]
        if not before:
            misses += 1
            continue
        base = before[-1][2]
        react = next((x for x in fr if x[0] >= t and angle(base, x[2]) > thr), None)
        if react is None or react[0] - t > 500e6:
            misses += 1
            continue
        r = react[1]
        for k, col in (("eval", "cpu_eval_ns"), ("gpu_eval", "gpu_eval_ns"), ("frame", "gpu_frame_ns"),
                       ("fg", "gpu_fg_ns"), ("first", "first_present_ns"), ("real", "real_present_ns")):
            v = int(r[col])
            if v and not (k == "fg" and int(r["n_gen"]) == 0):
                stages[k].append((v - t) / 1e6)
    print(f"injections: {len(ts)}, reactions: {len(stages['eval'])}, misses: {misses}")
    print(f"{'stage (ms after input)':<24}{'median':>8}{'mean':>8}{'p10':>8}{'p90':>8}")
    names = {"eval": "eval recorded (CPU)", "gpu_eval": "DLSS ran (GPU)", "frame": "frame done (GPU)",
             "fg": "DLSS-G done (GPU)", "first": "first present", "real": "real present"}
    for k, v in stages.items():
        if not v:
            continue
        v = sorted(v)
        q = lambda p: v[min(len(v) - 1, int(p * len(v)))]
        print(f"{names[k]:<24}{statistics.median(v):8.1f}{statistics.mean(v):8.1f}{q(.1):8.1f}{q(.9):8.1f}")


if __name__ == "__main__":
    main()
