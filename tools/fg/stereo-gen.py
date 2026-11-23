#!/usr/bin/env python3
"""Generated frames of stereo + frame generation, per eye (`quiver lab call <i> fg grab N TAG`: the real
frame and every generated frame of N consecutive jobs, whole, as THRW .raw; src/features/fg/fg.cpp).

DLSS-G's generated frame k of m between real frames R(n-1) and R(n) should, for EACH eye:
  - sit at k/m of the way: the eye half's horizontal image shift R(n-1)->G over R(n-1)->R(n) (phase
    correlation; meaningful while the camera turns, tools/fg/spin.py), the same for both eyes;
  - keep that eye's own view: per-patch stereo disparity of G's halves equal to the real frames'
    (a generated eye that pulled in the other eye's image loses disparity), and no seam artefacts;
  - be close to the nearer real frame of the same eye after the expected shift ("err same eye"), and far
    from the other eye's ("err other eye", the cross-eye ghost check: the two differ by the disparity).

usage: stereo-gen.py DIR [--tag TAG] [--stride 48] [--json out.json]
"""
import argparse
import glob
import json
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../re/scripts"))
from stereo_sbs import raw_load, field  # noqa: E402


def lum(path):
    a, fmt = raw_load(path)
    if fmt == 10:  # scRGB: linear -> display-ish gamma for matching
        a = np.clip(a, 0, None) ** (1 / 2.2)
    a = np.clip(np.nan_to_num(a[..., :3]), 0, None)
    y = a @ np.array([0.2126, 0.7152, 0.0722])
    return 255.0 * np.clip(y, 0, 1)


def shift_x(a, b, rows):
    """Horizontal shift of b relative to a (px, + = content moved right), phase correlation."""
    A = a[rows] - a[rows].mean()
    B = b[rows] - b[rows].mean()
    win = np.hanning(A.shape[1])[None, :] * np.hanning(A.shape[0])[:, None]
    FA, FB = np.fft.fft2(A * win), np.fft.fft2(B * win)
    R = FB * np.conj(FA)
    R /= np.abs(R) + 1e-9
    c = np.fft.ifft2(R).real
    y, x = np.unravel_index(np.argmax(c), c.shape)
    w = c.shape[1]
    # sub-pixel (parabola along x)
    l, m, r = c[y, (x - 1) % w], c[y, x], c[y, (x + 1) % w]
    d = 0.5 * (l - r) / (l - 2 * m + r) if (l - 2 * m + r) != 0 else 0.0
    sx = x + d
    if sx > w / 2:
        sx -= w
    return float(sx), float(m)


def roll(a, dx):
    i = int(round(dx))
    out = np.roll(a, i, axis=1)
    if i > 0:
        out[:, :i] = np.nan
    elif i < 0:
        out[:, i:] = np.nan
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--tag", default=None)
    ap.add_argument("--stride", type=int, default=48)
    ap.add_argument("--no-disp", action="store_true")
    ap.add_argument("--json")
    a = ap.parse_args()
    frames = {}
    for p in glob.glob(os.path.join(a.dir, "fggrab_*.raw")):
        m = re.match(r"fggrab_(\d+)_(.+)_(R|G(\d+)of(\d+))\.raw$", os.path.basename(p))
        if not m or (a.tag and m.group(2) != a.tag):
            continue
        n = int(m.group(1))
        frames.setdefault(n, {})["R" if m.group(3) == "R" else (int(m.group(4)), int(m.group(5)))] = p
    ns = sorted(frames)
    out = []
    cache = {}

    def L(p):
        if p not in cache:
            cache[p] = lum(p)
        return cache[p]

    for n in ns:
        if n - 1 not in frames or "R" not in frames[n - 1] or "R" not in frames[n]:
            continue
        A, C = L(frames[n - 1]["R"]), L(frames[n]["R"])
        H, W2 = A.shape
        W = W2 // 2
        rows = slice(int(H * 0.15), int(H * 0.55))  # above the player and the HUD hint
        eye = lambda img, e: img[:, e * W:(e + 1) * W]
        tot = [shift_x(eye(A, e), eye(C, e), rows)[0] for e in (0, 1)]
        disp_real = None
        if not a.no_disp:
            fa = field(eye(C, 0), eye(C, 1), stride=a.stride)
            disp_real = fa
        for key in sorted(k for k in frames[n] if k != "R"):
            k, mm = key
            G = L(frames[n][key])
            row = {"frame": n, "index": f"{k}/{mm}", "expected": round(k / mm, 3)}
            for e, name in ((0, "L"), (1, "R")):
                s, peak = shift_x(eye(A, e), eye(G, e), rows)
                row[f"shift_{name}"] = round(s, 2)
                row[f"total_{name}"] = round(tot[e], 2)
                row[f"ratio_{name}"] = round(s / tot[e], 3) if abs(tot[e]) > 2 else None
                # G vs the same eye's / the other eye's real frame n, moved back by the remaining shift
                back = -(tot[e] - s)
                same = roll(eye(C, e), back)
                other = roll(eye(C, 1 - e), back)
                g = eye(G, e)
                inner = (slice(int(H * 0.1), int(H * 0.9)), slice(64, W - 64))
                row[f"err_same_{name}"] = round(float(np.nanmean(np.abs(g - same)[inner])), 2)
                row[f"err_other_{name}"] = round(float(np.nanmean(np.abs(g - other)[inner])), 2)
            # seam: mean |G - R(n) moved| in 32-px column bands either side of x = W vs further in
            if disp_real is not None:
                fg_ = field(eye(G, 0), eye(G, 1), stride=a.stride)
                common = [p for p in fg_ if p in disp_real]
                dg = np.array([fg_[p] for p in common])
                dr = np.array([disp_real[p] for p in common])
                row["disp_patches"] = len(common)
                if len(common):
                    row["disp_median_G"] = round(float(np.median(dg)), 2)
                    row["disp_median_real"] = round(float(np.median(dr)), 2)
                    row["disp_absdiff_mean"] = round(float(np.mean(np.abs(dg - dr))), 2)
                    row["disp_absdiff_median"] = round(float(np.median(np.abs(dg - dr))), 2)
            out.append(row)
            print(json.dumps(row))
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
