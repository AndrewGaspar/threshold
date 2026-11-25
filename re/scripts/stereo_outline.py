#!/usr/bin/env python3
"""Where a thin bright outline (the Launch / aim targeting outline) sits against its object, per eye.

  stereo_outline.py [--box x0,y0,x1,y1] SBS.bmp...

The outline = thin bright lines (gray minus its 9x9 median > 45) inside the box (left-eye pixel
coordinates; default: the central region). Printed per capture:
  - the outline's disparity (shift L->R maximising the masks' overlap) and the object's (patch match
    of the box with the outline masked out); disparity = x_left - x_right;
  - per eye, the horizontal shift that moves the outline onto the object's silhouette edges (the
    gradient of the median-filtered image, which has no thin outline); 0 = on the object,
    + = the object is to the right of the outline.
"""
import sys
import numpy as np
from PIL import Image, ImageFilter


def main():
    args = sys.argv[1:]
    box = None
    if args and args[0] == "--box":
        box = list(map(int, args[1].split(",")))
        args = args[2:]
    for f in args:
        im = Image.open(f).convert("L")
        W = im.width // 2
        g = np.asarray(im).astype(float)
        med = np.asarray(im.filter(ImageFilter.MedianFilter(9))).astype(float)
        th = (g - med) > 45
        edges = np.hypot(*np.gradient(med))
        bx = box or [400, 250, 1500, 1000]
        reg = np.zeros((g.shape[0], W), bool)
        reg[bx[1]:bx[3], bx[0]:bx[2]] = True
        ys, xs = np.nonzero(th[:, :W] & reg)
        name = f.split("/")[-1]
        if len(xs) < 100:
            print(f"{name}: no outline ({len(xs)} px)")
            continue
        x0, x1 = max(xs.min() - 20, 80), min(xs.max() + 20, W - 80)
        y0, y1 = ys.min() - 10, ys.max() + 10
        w = x1 - x0
        mL = th[y0:y1, x0:x1].astype(float)
        bo = bg = None
        for s in range(-60, 61):
            mR = th[y0:y1, W + x0 - s:W + x1 - s].astype(float)
            ov = (mL * mR).sum() / max(1.0, np.sqrt(mL.sum() * mR.sum()))
            if bo is None or ov > bo[1]:
                bo = (s, ov)
            keep = (mL == 0) & (mR == 0)
            d = np.abs(g[y0:y1, x0:x1] - g[y0:y1, W + x0 - s:W + x1 - s])[keep].mean()
            if bg is None or d < bg[1]:
                bg = (s, d)

        def eye_shift(m, ex0):
            best = None
            for t in range(-30, 31):
                sub = edges[y0:y1, ex0 + t:ex0 + t + w]
                sc = (sub * m).sum() / max(1.0, m.sum())
                if best is None or sc > best[1]:
                    best = (t, sc)
            return best[0]

        mR = th[y0:y1, W + x0 - bo[0]:W + x1 - bo[0]].astype(float)
        eL = eye_shift(mL, x0)
        eR = eye_shift(mR, W + x0 - bo[0])
        print(f"{name}: box x {x0}-{x1} y {y0}-{y1}, outline {int(mL.sum())} px: outline disparity {bo[0]} "
              f"(overlap {bo[1]:.2f}), object disparity {bg[0]} (|diff| {bg[1]:.1f}); outline -> object: "
              f"left {eL:+d} px, right {eR:+d} px")


if __name__ == "__main__":
    main()
