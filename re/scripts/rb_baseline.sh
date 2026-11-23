#!/usr/bin/env bash
# rb_baseline.sh MODDIR [SECS]: like stereo_rb_cinematic.sh but needs nothing from our mod (works with an
# empty MODDIR: the game alone). Continue, let the opening play, then SECS (default 480) after the
# player gets control, checking every ~20 s that frames still come; prints the hang evidence.
set -uo pipefail
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:threshold-stereo} QUIVER_LAB_OWNER=${QUIVER_LAB_OWNER:-claude:threshold-stereo}
mod=$1 secs=${2:-480} i=control-stereo
q() { quiver --machine rayblaster lab "$@"; }
q kill $i >/dev/null 2>&1
q fixture install $i game-start >/dev/null 2>&1
ssh -o BatchMode=yes rayblaster 'chmod -R u+w ~/Games/quiver-lab/instances/control-stereo/prefix/drive_c/users/steamuser/Documents/My\ Games/Control/Saves'
tl=$(date +%s)
q launch $i --mod "$mod" --wait frame --timeout 400 | tail -1
for k in Return Return Escape Return; do q shot $i /dev/null --settle 5 >/dev/null 2>&1; q key $i $k >/dev/null; done
# without our mod there is no lab state or `where`: the DLC panel is dismissed blind, and control is
# taken to start ~440 s after launch (the cinematic ends at ~431 s of process time in every run).
q shot $i /dev/null --settle 20 >/dev/null 2>&1; q key $i Escape >/dev/null; q shot $i /dev/null --settle 3 >/dev/null 2>&1; q key $i Return >/dev/null
t0=""
for n in $(seq 1 200); do
  if ! q wait $i frame --timeout 15 >/dev/null 2>&1; then echo "$(date +%T) no frames: hung"; break; fi
  q shot $i /dev/null --settle 15 >/dev/null 2>&1
  if [ -z "$t0" ] && { q where $i 2>/dev/null | grep -q '"control":true' || [ $(( $(date +%s) - tl )) -ge 440 ]; }; then t0=$(date +%s); echo "$(date +%T) control"; fi
  [ -n "$t0" ] && [ $(( $(date +%s) - t0 )) -ge "$secs" ] && { echo "$(date +%T) passed: $secs s of control"; break; }
done
ssh -o BatchMode=yes rayblaster 'L=$(ls -dt ~/Games/quiver-lab/instances/control-stereo/lab/runs/r* | head -1); echo run $L; journalctl -k --since "-20min" | grep "Xid.*Control" | tail -1'
