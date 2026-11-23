#!/usr/bin/env bash
# stereo_battery.sh INSTANCE OUTDIR: Threshold stereo captures for docs/stereo.md, in the current scene
# (game in the world, [stereo] output = full, mode sbs). Each capture is the whole presented back
# buffer (`stereo shots`), so both halves of an sbs capture come from one game update.
#   mono L/R   mode left / right (mono-frame bias; separate frames), reference disparity field
#   s0_*       sbs, separation 0 (control: any difference is state crossing eyes or a render side
#              effect), per-eye state on / off
#   s32_*      sbs, 32.5 mm, parallel: per-eye state on / off / DLSS shared
#   c4_*       sbs, 32.5 mm, convergence 4 m (image shift)
set -uo pipefail
inst=$1 out=$2
mkdir -p "$out"
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:threshold-stereo}
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
  cp "/home/ajg/Games/quiver-lab/instances/$inst/lab/runs/$run/$name" "$out/$1.bmp" && echo "$out/$1.bmp"
}
set_ mode=left separation=32.5 convergence=0 per_eye=1 dlss_per_eye=1; cap monoL
set_ mode=right; cap monoR
set_ mode=sbs separation=0; cap s0_pe1a; cap s0_pe1b
set_ per_eye=0 dlss_per_eye=0; cap s0_pe0a; cap s0_pe0b
set_ separation=32.5 per_eye=1 dlss_per_eye=1; cap s32_pe1a; cap s32_pe1b
set_ per_eye=0 dlss_per_eye=0; cap s32_pe0a; cap s32_pe0b
set_ per_eye=1 dlss_per_eye=0; cap s32_dlss0a
set_ dlss_per_eye=1 convergence=4; cap c4a
set_ convergence=0; cap s32_pe1c
