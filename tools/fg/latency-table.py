#!/usr/bin/env python3
"""One docs/fg.md row per tools/fg/latency result directory: rendered fps, generated per frame,
median (p10-p90) ms from input to eval recorded / frame done / first present / real present, the
game's waits (perf line) and the host's GPU load.

usage: latency-table.py <dir>...
"""
import re
import sys

print("| run | fps | gen/frame | eval recorded | frame done (GPU) | first present | real present | waits /2 s | host GPU |")
print("|---|---|---|---|---|---|---|---|---|")
for d in sys.argv[1:]:
    rep = open(f"{d}/report.txt").read()
    m = re.search(r"= ([\d.]+) rendered fps, ([\d.]+) generated", rep)
    fps, gen = (m.group(1), m.group(2)) if m else ("?", "?")
    cells = []
    for name in ("eval recorded", "frame done", "first present", "real present"):
        m = re.search(name + r"[^\n]*?\s+([\d.]+)\s+[\d.]+\s+([\d.]+)\s+([\d.]+)", rep)
        cells.append(f"{m.group(1)} ({m.group(2)}-{m.group(3)})" if m else "-")
    waits = "?"
    try:
        m = re.search(r"game waited (\d+) x", open(f"{d}/perf.txt").read().splitlines()[-1])
        waits = m.group(1) if m else "?"
    except (OSError, IndexError):
        pass
    host = ""
    try:
        m = re.search(r"RTX \d+ (\d+)%.*?gpu users: (.*)", open(f"{d}/host.txt").read().splitlines()[0])
        if m:
            others = [u for u in re.findall(r"\(lab:([\w-]+)\)", m.group(2)) if u != "control-fg"]
            host = f"{m.group(1)}%, {' '.join(others)}"
    except (OSError, IndexError):
        pass
    print(f"| {d.rstrip('/').rsplit('/', 1)[-1]} | {fps} | {gen} | " + " | ".join(cells) + f" | {waits} | {host} |")
