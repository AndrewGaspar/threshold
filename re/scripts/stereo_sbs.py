#!/usr/bin/env python3
"""Analyse Threshold stereo captures (`quiver lab call <i> stereo shots N TAG`, 24-bit BMP of the whole
back buffer). Each side-by-side capture is split into its left and right halves.

  stereo_sbs.py diff A.bmp [B.bmp ...]          |L-R| per capture (separation-0 control)
  stereo_sbs.py disp A.bmp [--ref L.bmp R.bmp]  per-patch disparity of the halves (and, with a
                                                reference pair, per-patch difference to it)
  stereo_sbs.py pair L.bmp R.bmp                disparity between two captures' left halves
  stereo_sbs.py refl A.bmp [...] --ref L.bmp R.bmp  each half against its eye's mono-frame reference:
                                                mean |diff| (0-255) per --region (whole image if none)
  stereo_sbs.py raw A.raw [--png out.png]       stats of a raw texture grab (`stereo grab`, `shots N
                                                TAG-with-raw`): per channel min/mean/max, NaN/Inf counts
Options: --m00 (projection m00, for implied depth), --eye (per-eye offset m), --region name:x0,y0,x1,y1
"""
import argparse
import json
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stereo_disparity import match  # noqa: E402


def halves(path):
    a = np.asarray(Image.open(path).convert("L"), dtype=np.float64)
    w = a.shape[1] // 2
    return a[:, :w], a[:, w:]


def rgb_halves(path):
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    w = a.shape[1] // 2
    return a[:, :w], a[:, w:]


DXGI = {10: ("<f2", 4), 2: ("<f4", 4), 24: ("r10g10b10a2", 4), 26: ("r11g11b10", 3), 28: ("u1", 4),
        87: ("u1", 4), 41: ("<f4", 1), 54: ("<f2", 1), 56: ("<u2", 1), 61: ("u1", 1)}


def raw_load(path):
    """A `stereo grab` .raw: header THRW, w, h, pitch, DXGI format; float array (h, w, c)."""
    b = open(path, "rb").read()
    tag, w, h, pitch, fmt = np.frombuffer(b[:20], "<u4")
    assert tag == 0x57524854, "not a THRW file"
    rows = np.frombuffer(b[20:20 + pitch * h], np.uint8).reshape(h, pitch)
    kind, c = DXGI.get(int(fmt), (None, 0))
    if kind is None:
        raise SystemExit("DXGI format %d not decoded" % fmt)
    if kind in ("r10g10b10a2", "r11g11b10"):
        p = rows[:, :w * 4].copy().view("<u4").astype(np.uint64)
        if kind == "r10g10b10a2":
            a = np.stack([(p & 0x3ff), (p >> 10) & 0x3ff, (p >> 20) & 0x3ff], -1).astype(np.float64) / 1023
        else:
            def f(v, mb):
                e = (v >> mb).astype(np.int64); m = (v & ((1 << mb) - 1)).astype(np.float64)
                out = np.where(e == 0, 0.0, np.ldexp(1 + m / (1 << mb), e - 15))
                return np.where(e == 31, np.where(m > 0, np.nan, np.inf), out)
            a = np.stack([f(p & 0x7ff, 6), f((p >> 11) & 0x7ff, 6), f((p >> 22) & 0x3ff, 5)], -1)
        return a, int(fmt)
    dt = np.dtype(kind)
    a = rows[:, :w * c * dt.itemsize].copy().view(dt).reshape(h, w, c).astype(np.float64)
    if kind == "u1":
        a = a / 255
    return a, int(fmt)


def raw_stats(path, png=None):
    a, fmt = raw_load(path)
    out = {"file": os.path.basename(path), "fmt": fmt, "shape": list(a.shape)}
    for i in range(min(a.shape[2], 4)):
        ch = a[..., i]
        fin = ch[np.isfinite(ch)]
        out["c%d" % i] = {"nan": int(np.isnan(ch).sum()), "inf": int(np.isinf(ch).sum()),
                          "min": round(float(fin.min()), 5) if fin.size else None,
                          "mean": round(float(fin.mean()), 5) if fin.size else None,
                          "max": round(float(fin.max()), 5) if fin.size else None}
    if png:
        v = np.nan_to_num(a[..., :3] if a.shape[2] >= 3 else np.repeat(a[..., :1], 3, -1), nan=0, posinf=1)
        if a.shape[2] == 1:
            v = v / max(float(np.percentile(v, 99)), 1e-6)
        v = np.clip(v, 0, 1) ** (1 / 2.2)
        Image.fromarray((v * 255 + 0.5).astype(np.uint8)).save(png)
    return out


def field(L, R, patch=32, stride=24, smax=96, minc=0.5):
    H, W = L.shape
    pts = {}
    for y in range(0, H - patch, stride):
        for x in range(0, W - patch, stride):
            m = match(L, R, x, y, patch, smax)
            if m and m[1] >= minc:
                pts[(x, y)] = m[0]
    return pts


def summary(pts, regions, K):
    d = np.array(list(pts.values())) if pts else np.zeros(0)
    out = {"patches": int(len(d))}
    if len(d):
        out.update(median=round(float(np.median(d)), 3), p5=round(float(np.percentile(d, 5)), 2),
                   p95=round(float(np.percentile(d, 95)), 2))
    for r in regions:
        name, box = r.split(":")
        x0, y0, x1, y1 = [int(v) for v in box.split(",")]
        sel = [v for (x, y), v in pts.items() if x0 <= x + 16 < x1 and y0 <= y + 16 < y1]
        if sel:
            md = float(np.median(sel))
            out[name] = {"n": len(sel), "median": round(md, 2),
                         "depth_m": round(K / md, 2) if K and md > 0.05 else None}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["diff", "disp", "pair", "refl", "raw"])
    ap.add_argument("--png", default=None)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--ref", nargs=2, default=None)
    ap.add_argument("--m00", type=float, default=0.0)
    ap.add_argument("--eye", type=float, default=0.0325)
    ap.add_argument("--region", action="append", default=[])
    a = ap.parse_args()
    if a.cmd == "diff":
        for f in a.files:
            L, R = rgb_halves(f)
            d = np.abs(L - R)
            print(json.dumps({"file": os.path.basename(f), "mean": round(float(d.mean()), 4),
                              "max": int(d.max()), "gt2_pct": round(float((d.max(axis=2) > 2).mean() * 100), 3)}))
        return
    if a.cmd == "raw":
        for f in a.files:
            print(json.dumps(raw_stats(f, a.png)))
        return
    if a.cmd == "refl":
        L0, _ = rgb_halves(a.ref[0])
        R0, _ = rgb_halves(a.ref[1])
        regions = a.region or ["all:0,0,%d,%d" % (L0.shape[1], L0.shape[0])]
        for f in a.files:
            L, R = rgb_halves(f)
            out = {"file": os.path.basename(f)}
            for r in regions:
                name, box = r.split(":")
                x0, y0, x1, y1 = [int(v) for v in box.split(",")]
                dl = np.abs(L[y0:y1, x0:x1] - L0[y0:y1, x0:x1]).mean()
                dr = np.abs(R[y0:y1, x0:x1] - R0[y0:y1, x0:x1]).mean()
                out[name] = {"L": round(float(dl), 3), "R": round(float(dr), 3)}
            print(json.dumps(out))
        return
    if a.cmd == "pair":
        L, _ = halves(a.files[0])
        R, _ = halves(a.files[1])
        pairs = [(os.path.basename(a.files[0]) + "|" + os.path.basename(a.files[1]), L, R)]
    else:
        pairs = [(os.path.basename(f),) + halves(f) for f in a.files]
    W = pairs[0][1].shape[1]
    K = 2 * a.eye * a.m00 * (W / 2) if a.m00 else 0.0
    ref = None
    if a.ref:
        L0, _ = halves(a.ref[0])
        R0, _ = halves(a.ref[1])
        ref = field(L0, R0)
    for name, L, R in pairs:
        pts = field(L, R)
        out = {"file": name, "K_px_m": round(K, 2), **summary(pts, a.region, K)}
        if ref:
            common = [pts[k] - ref[k] for k in pts if k in ref]
            if common:
                c = np.array(common)
                out["vs_ref"] = {"n": len(c), "median": round(float(np.median(c)), 3),
                                 "mean_abs": round(float(np.abs(c).mean()), 3)}
        print(json.dumps(out))


if __name__ == "__main__":
    main()
