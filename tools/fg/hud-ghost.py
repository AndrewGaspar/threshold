#!/usr/bin/env python3
"""HUD ghosting in generated frames, from a Threshold present trace whose band covers a HUD element
(`quiver lab call <i> fg trace N S <y> <h>`, tools/fg/fg-trace ... with the band on the HUD).

For every run R(N-1) G1..Gk R(N) of a camera spin: in the HUD box, the HUD's own pixels (bright in
both real frames: the glyphs) should be the same in the generated frames, since the HUD doesn't move.
Without a HUD-less input DLSS-G interpolates them with the scene's motion (ghosting / smearing).

Reports, per generated frame, the mean |G - mean(R_prev, R_next)| over (a) the glyph pixels of the HUD
box, (b) the whole HUD box, (c) a scene box of the same size, plus the same between the two real
frames for reference.

usage: hud-ghost.py <dir> --hud x0 x1 [--scene x0 x1] [--thr 0.7]
"""
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from importlib import import_module  # noqa: E402

fp = import_module("fg-pacing")


def arg(name, n, default=None):
    if name in sys.argv:
        i = sys.argv.index(name)
        return [float(v) for v in sys.argv[i + 1:i + 1 + n]]
    return default


def main():
    d = sys.argv[1]
    rows, bands = fp.load_rgb(d)
    hx = arg("--hud", 2)
    sx = arg("--scene", 2, [hx[0] + 700, hx[1] + 700])
    thr = arg("--thr", 1, [0.7])[0]
    shot = sorted([r for r in rows if r["shot"] >= 0], key=lambda r: r["shot"])
    hb = slice(int(hx[0]), int(hx[1]))
    sb = slice(int(sx[0]), int(sx[1]))
    g_glyph, g_box, g_scene, r_glyph, r_box, r_scene, nglyph = [], [], [], [], [], [], []
    for i in range(len(shot) - 2):
        a = shot[i]
        if a["kind"] != "R" or shot[i + 1]["kind"] != "G":
            continue
        j = i + 1
        while j < len(shot) and shot[j]["kind"] == "G":
            j += 1
        if j >= len(shot) or shot[j]["kind"] != "R":
            continue
        rp = bands[a["shot"]] / 255.0
        rn = bands[shot[j]["shot"]] / 255.0
        ref = 0.5 * (rp + rn)
        lum_p, lum_n = rp[:, hb].mean(-1), rn[:, hb].mean(-1)
        mask = (lum_p > thr) & (lum_n > thr)
        if mask.sum() < 20:
            continue
        nglyph.append(int(mask.sum()))
        r_glyph.append(np.abs(rn[:, hb] - rp[:, hb]).mean(-1)[mask].mean())
        r_box.append(np.abs(rn[:, hb] - rp[:, hb]).mean())
        r_scene.append(np.abs(rn[:, sb] - rp[:, sb]).mean())
        for g in shot[i + 1:j]:
            gi = bands[g["shot"]] / 255.0
            g_glyph.append(np.abs(gi[:, hb] - ref[:, hb]).mean(-1)[mask].mean())
            g_box.append(np.abs(gi[:, hb] - ref[:, hb]).mean())
            g_scene.append(np.abs(gi[:, sb] - ref[:, sb]).mean())
    if not g_glyph:
        print(f"{d}: no R-G-R runs with HUD glyphs in the box (threshold {thr})")
        return

    def s(v):
        v = np.asarray(v)
        return f"mean {v.mean():.4f} p50 {np.median(v):.4f} p90 {np.percentile(v, 90):.4f}"

    print(f"{d}: {len(g_glyph)} generated frames, {len(r_glyph)} real pairs, glyph px per frame ~{int(np.median(nglyph))}")
    print(f"  |G - mean(R,R')| glyph pixels: {s(g_glyph)}")
    print(f"  |R' - R|         glyph pixels: {s(r_glyph)}")
    print(f"  |G - mean(R,R')| HUD box:      {s(g_box)}")
    print(f"  |G - mean(R,R')| scene box:    {s(g_scene)}")
    print(f"  |R' - R|         scene box:    {s(r_scene)}")


if __name__ == "__main__":
    main()
