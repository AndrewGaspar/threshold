#!/usr/bin/env bash
# stereo_rb_photo.sh MODDIR [hdr]: launch rayblaster's control-stereo with MODDIR, drive into the lobby
# and freeze it in Photo Mode with its UI hidden (the world keeps rendering, nothing moves), for A/B
# captures (RB=rayblaster re/scripts/stereo_ab.sh control-stereo OUT ...).
set -uo pipefail
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:threshold-stereo} QUIVER_LAB_OWNER=${QUIVER_LAB_OWNER:-claude:threshold-stereo}
mod=$1 hdr=${2:-} i=control-stereo
q() { quiver --machine rayblaster lab "$@"; }
q kill $i >/dev/null 2>&1
q fixture install $i game-start >/dev/null 2>&1
ssh -o BatchMode=yes rayblaster 'chmod -R u+w ~/Games/quiver-lab/instances/control-stereo/prefix/drive_c/users/steamuser/Documents/My\ Games/Control/Saves'
q launch $i --mod "$mod" --wait frame --timeout 400 | tail -1
q shot $i /dev/null --settle 6 >/dev/null 2>&1
[ -n "$hdr" ] && q click $i 0.108 0.786 >/dev/null 2>&1
for k in Return Return Escape Return; do q shot $i /dev/null --settle 5 >/dev/null 2>&1; q key $i $k >/dev/null; done
if ! q wait $i in-world --timeout 240 >/dev/null 2>&1; then
  q key $i Escape >/dev/null; q shot $i /dev/null --settle 3 >/dev/null 2>&1; q key $i Return >/dev/null
  q wait $i in-world --timeout 240 >/dev/null 2>&1
fi
q key $i space --hold 1500 >/dev/null
for n in $(seq 1 15); do q key $i space >/dev/null; q shot $i /dev/null --settle 6 >/dev/null 2>&1; q where $i 2>/dev/null | grep -q '"control":true' && break; done
for n in $(seq 1 20); do
  q key $i Escape >/dev/null; q shot $i /dev/null --settle 3 >/dev/null 2>&1
  q where $i 2>/dev/null | grep -q '"menu":true' && break
  q shot $i /dev/null --settle 6 >/dev/null 2>&1
done
q key $i Down Down Return >/dev/null; q shot $i /dev/null --settle 3 >/dev/null 2>&1; q key $i t >/dev/null
q shot $i --settle 3 2>&1 | tail -1
