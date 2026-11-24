#!/usr/bin/env python3
"""Left/right differences in side-by-side captures (`stereo shots`).

  stereo_lr.py lr BMP...                 |L-R| per capture (with separation 0 the eyes should match):
                                         mean, pixels > 12/255 and their bbox; writes *_LR.png
  stereo_lr.py shift BMP x0 y0 x1 y1     best horizontal shift of the right eye's box against the
                                         left's (disparity = x_left - x_right) and the mean |diff|
                                         there and at 0
"""
import sys
import numpy as np
from PIL import Image


def load(f):
    a = np.asarray(Image.open(f).convert("RGB")).astype(np.float32)
    return a, a.shape[1] // 2


def lr(files):
    for f in files:
        a, W = load(f)
        d = np.abs(a[:, :W] - a[:, W:]).mean(-1)
        ys, xs = np.nonzero(d > 12)
        bb = f"bbox x {xs.min()}-{xs.max()} y {ys.min()}-{ys.max()}" if len(xs) else ""
        print(f"{f.split('/')[-1]}: mean {d.mean():.3f} px>12 {len(xs)} {bb}")
        Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(f.rsplit(".", 1)[0] + "_LR.png")


def shift(f, x0, y0, x1, y1):
    a, W = load(f)
    L = a[y0:y1, x0:x1]
    best = None
    for s in range(-60, 61):
        if x0 - s < 0 or x1 - s > W:
            continue
        e = np.abs(L - a[y0:y1, W + x0 - s:W + x1 - s]).mean()
        if best is None or e < best[1]:
            best = (s, e)
    e0 = np.abs(L - a[y0:y1, W + x0:W + x1]).mean()
    print(f"{f.split('/')[-1]}: disparity {best[0]} px, |diff| {best[1]:.2f} (at 0: {e0:.2f})")


if __name__ == "__main__":
    if sys.argv[1] == "lr":
        lr(sys.argv[2:])
    elif sys.argv[1] == "shift":
        shift(sys.argv[2], *map(int, sys.argv[3:7]))
    else:
        sys.exit(__doc__)
