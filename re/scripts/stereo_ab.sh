#!/usr/bin/env bash
# stereo_ab.sh INSTANCE OUTDIR [-n N] TAG:key=v,key=v ... : captures for an A/B of [stereo] settings in
# the current scene. First the mono-frame references (mode left / right: monoL.bmp, monoR.bmp), then
# for each variant its settings (live) and N captures TAG_1.bmp .. TAG_N.bmp of the presented buffer
# (mode sbs unless the variant sets it). Analyse with stereo_sbs.py disp/refl/diff --ref monoL monoR.
# Variants never inherit: each starts from the base below plus its own keys.
# RB=rayblaster runs it against that machine's instance (quiver --machine; captures fetched with scp).
set -uo pipefail
inst=$1 out=$2; shift 2
n=2
if [ "${1:-}" = "-n" ]; then n=$2; shift 2; fi
mkdir -p "$out"
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:threshold-stereo}
Q=(quiver); [ -n "${RB:-}" ] && Q=(quiver --machine "$RB")
quiver() { command "${Q[@]}" "$@"; }
base="mode=sbs separation=32.5 convergence=0 per_eye=1 dlss_per_eye=1 history_mask=30 graph_cross=0"
set_() { quiver lab set "$inst" $(for kv in "$@"; do echo "install:threshold.ini:[stereo]$kv"; done) --wait >/dev/null 2>&1
         quiver lab shot "$inst" /dev/null --settle 3 >/dev/null 2>&1; }
cap() {
  quiver lab call "$inst" stereo shots 1 "$1" >/dev/null
  local f=""
  for i in $(seq 1 40); do
    f=$(quiver lab call "$inst" stereo files 2>/dev/null | tr -d ' \n[]"')
    [ -n "$f" ] && break
    quiver lab wait "$inst" frame --timeout 2 >/dev/null 2>&1
  done
  local run=$(echo "$f" | sed -E 's/.*runs\\\\(r[0-9]+)\\\\.*/\1/') name=$(echo "$f" | sed -E 's/.*\\\\//')
  if [ -n "${RB:-}" ]; then scp -q "$RB:Games/quiver-lab/instances/$inst/lab/runs/$run/$name" "$out/$1.${name##*.}"
  else cp "/home/ajg/Games/quiver-lab/instances/$inst/lab/runs/$run/$name" "$out/$1.${name##*.}"; fi && echo "$out/$1.${name##*.}"
}
set_ $base mode=left; cap monoL
set_ $base mode=right; cap monoR
for v in "$@"; do
  tag=${v%%:*} kv=${v#*:}
  set_ $base ${kv//,/ }
  for i in $(seq 1 "$n"); do cap "${tag}_$i"; done
done
set_ $base
