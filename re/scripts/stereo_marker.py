#!/usr/bin/env python3
"""Disparity of a HUD marker against the scene around it, in a side-by-side capture (Threshold).

  stereo_marker.py SBS.png --box x0,y0,x1,y1 [--thresh 235] [--ref x0,y0,x1,y1 ...]

--box (left-eye pixels) bounds the marker: its bright pixels (min channel >= thresh) are taken in the
left half and the same box (widened by --search px) in the right half; the disparity is the
difference of their centroids, x_left - x_right in px (as stereo_disparity.py: 2K/z minus the
convergence shift, so larger = nearer). --ref boxes are scene patches matched between the halves by SSD for comparison:
a marker at its anchor's depth has the disparity of the scene at that depth.
"""
import argparse
import numpy as np
from PIL import Image


def centroid(a, thresh):
    m = a.min(axis=2) >= thresh
    ys, xs = np.nonzero(m)
    if len(xs) < 3:
        return None, 0
    return (xs.mean(), ys.mean()), len(xs)


def patch_disp(L, R, box, smax=60):
    x0, y0, x1, y1 = box
    ref = R[y0:y1, x0:x1]
    best = None
    for d in range(-smax, smax + 1):
        if x0 + d < 0 or x1 + d > L.shape[1]:
            continue
        e = ((L[y0:y1, x0 + d:x1 + d] - ref) ** 2).mean()
        if best is None or e < best[0]:
            best = (e, d)
    e, d = best
    def err(k):
        return ((L[y0:y1, x0 + k:x1 + k] - ref) ** 2).mean()
    a, b, c = err(d - 1), err(d), err(d + 1)
    den = a - 2 * b + c
    return d + (0.5 * (a - c) / den if den > 0 else 0.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sbs")
    ap.add_argument("--box", required=True)
    ap.add_argument("--thresh", type=int, default=235)
    ap.add_argument("--search", type=int, default=60)
    ap.add_argument("--ref", action="append", default=[])
    a = ap.parse_args()
    im = np.asarray(Image.open(a.sbs).convert("RGB"), dtype=np.float64)
    W = im.shape[1] // 2
    Lc, Rc = im[:, :W], im[:, W:]
    x0, y0, x1, y1 = map(int, a.box.split(","))
    s = a.search
    cl, nl = centroid(Lc[y0:y1, x0:x1], a.thresh)
    cr, nr = centroid(Rc[y0:y1, max(0, x0 - s):x1 + s], a.thresh)
    if cl and cr:
        xl, xr = x0 + cl[0], max(0, x0 - s) + cr[0]
        print(f"marker: left x {xl:.2f} y {y0 + cl[1]:.2f} ({nl} px), right x {xr:.2f} y {y0 + cr[1]:.2f} ({nr} px), "
              f"disparity L-R {xl - xr:+.2f} px")
    else:
        print("marker: not found", nl, nr)
    Lg, Rg = Lc.mean(axis=2), Rc.mean(axis=2)
    for r in a.ref:
        b = tuple(map(int, r.split(",")))
        print(f"scene {r}: disparity L-R {patch_disp(Lg, Rg, b):+.2f} px")


if __name__ == "__main__":
    main()
