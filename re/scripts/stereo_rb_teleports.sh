#!/usr/bin/env bash
# stereo_rb_teleports.sh MODDIR [hdr]: the level-change hang test (docs/stereo.md "Level-change hang").
# Launches rayblaster's control-stereo with MODDIR (uploaded by quiver --machine), drives the menus into
# the world, then 6 teleports; one line per step, then the run's signaller-timeout count.
set -uo pipefail
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:threshold-stereo} QUIVER_LAB_OWNER=${QUIVER_LAB_OWNER:-claude:threshold-stereo}
mod=$1 hdr=${2:-} i=control-stereo
q() { quiver --machine rayblaster lab "$@"; }
q kill $i >/dev/null 2>&1
q fixture install $i game-start >/dev/null 2>&1
ssh -o BatchMode=yes rayblaster 'chmod -R u+w ~/Games/quiver-lab/instances/control-stereo/prefix/drive_c/users/steamuser/Documents/My\ Games/Control/Saves'
q launch $i --mod "$mod" --wait frame --timeout 400 | tail -1
q shot $i /dev/null --settle 6 >/dev/null 2>&1
[ -n "$hdr" ] && q click $i 0.108 0.786 >/dev/null 2>&1   # the Galaxy box ignores Return in HDR
for k in Return Return Escape Return; do q shot $i /dev/null --settle 5 >/dev/null 2>&1; q key $i $k >/dev/null; done
if ! q wait $i in-world --timeout 240 >/dev/null 2>&1; then   # the DLC promo panel came up late
  q key $i Escape >/dev/null; q shot $i /dev/null --settle 3 >/dev/null 2>&1; q key $i Return >/dev/null
  q wait $i in-world --timeout 240 >/dev/null 2>&1
fi
q key $i space --hold 1500 >/dev/null
for n in $(seq 1 15); do q key $i space >/dev/null; q shot $i /dev/null --settle 6 >/dev/null 2>&1; q where $i 2>/dev/null | grep -q '"control":true' && break; done
st() { q call $i stereo status 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print('pairs', d['pairs'], 'present', d['present'], 'hud_wide', d.get('hud_wide'), 'load_mono', d.get('load_mono'), 'changes', d.get('level_changes'), 'blas', d.get('blas_builds'))" 2>/dev/null; }
echo "start: $(st)"
for loc in mail-room executive-corridor cafeteria motel dead-letters entry-lobby; do
  r=$(q teleport $i $loc --timeout 150 2>&1 | tail -1 | cut -c1-60)
  echo "$loc: $r | $(st)"; echo "$r" | grep -q Error && break
done
if ! tail -1 <<<"$r" | grep -q Error; then   # then a checkpoint restart, and 40 s of play after it
  q call $i restart >/dev/null 2>&1; q wait $i in-world --timeout 240 >/dev/null 2>&1
  for n in $(seq 1 8); do q wait $i frame --timeout 6 >/dev/null 2>&1; q shot $i /dev/null --settle 4 >/dev/null 2>&1; done
  a=$(st); q wait $i frame --timeout 6 >/dev/null 2>&1; q shot $i /dev/null --settle 4 >/dev/null 2>&1; echo "restart: $a -> $(st)"
fi
ssh -o BatchMode=yes rayblaster 'L=$(ls -dt ~/Games/quiver-lab/instances/control-stereo/lab/runs/r* | head -1); echo run $L; echo signaller $(grep -c signaller $L/game.log); grep "RT async\|stall: no" $L/threshold.log | head -4; journalctl -k --since "-25min" | grep "Xid.*Control" | tail -2'
