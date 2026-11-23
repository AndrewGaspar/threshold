#!/usr/bin/env python3
"""One table row per fg-trace result (build-fg/fgtrace/<label>/result.json + perf.txt), for docs/fg.md.

usage: fg-table.py <dir>...
"""
import json
import re
import sys


def row(d):
    r = json.load(open(f"{d}/result.json"))
    perf = open(f"{d}/perf.txt").read().splitlines() if True else []
    real = pres = lag = waits = None
    for line in perf[-2:]:
        m = re.search(r"real ([\d.]+) \+ generated ([\d.]+) = ([\d.]+)", line)
        if m:
            real, pres = float(m.group(1)), float(m.group(3))
        m = re.search(r"DLSS-G done ([\d.]+) ms", line)
        if m:
            lag = float(m.group(1))
        m = re.search(r"game waited (\d+) x", line)
        if m:
            waits = int(m.group(1))
    if "gen_ratio_index" in r:
        ratio = ", ".join(f"{s['p50']:.3f} ({s['p10']:.3f}-{s['p90']:.3f})" for s in r["gen_ratio_index"].values())
    elif "gen_ratio" in r:
        s = r["gen_ratio"]
        ratio = f"{s['p50']:.3f} ({s['p10']:.3f}-{s['p90']:.3f})"
    else:
        ratio = "-"
    n = r.get("gen_ratio", {}).get("n", 0)
    out = f"{r.get('gen_ratio_inverted', '-')} of {n}"
    sd = r["interval_all"]["std"]
    vrr = r.get("display_vrr", {}).get("resid_abs_px", {})
    run = open(f"{d}/run.txt").read().strip()
    tr = " / ".join(f"{k} {r[f'interval_{k}']['std']:.2f}" for k in ("GG", "GR", "RG") if f"interval_{k}" in r)
    host = ""
    try:
        h = open(f"{d}/host.txt").read().splitlines()[0]
        m = re.search(r"gpu .*? (\d+)% (\d+)/\d+ MiB.*?gpu users: (.*)", h)
        if m:
            host = f"{m.group(1)}%, {m.group(3).replace('Control_DX12.exe ', '').replace('AlanWake2.exe ', 'AW2 ')}"
    except OSError:
        pass
    label = d.rstrip("/").rsplit("/", 1)[-1]
    print(f"| {label} | {run} | {real} | {pres} | {lag} | {waits} | {ratio} | {out} | {sd:.2f} ({tr}) | "
          f"{vrr.get('mean', 0):.2f} / {vrr.get('p90', 0):.2f} | {host} |")


print("| trace | run | real fps | presents/s | DLSS-G done after Present ms | game waited /2 s | shift ratio median (p10-p90) "
      "per index | outside [0,1] | present interval sd ms (G->G / G->R / R->G) | position residual mean / p90 px | host GPU |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for d in sys.argv[1:]:
    try:
        row(d)
    except Exception as e:  # noqa: BLE001
        print(f"| {d} | error {e} |")
