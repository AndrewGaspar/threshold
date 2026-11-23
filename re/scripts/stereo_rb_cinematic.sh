#!/usr/bin/env bash
# stereo_rb_cinematic.sh MODDIR: launch rayblaster's control-stereo with MODDIR, Continue, and let the
# opening cinematic play untouched (no keys) through its transitions until the player has control
# (~8 min); prints world-render counts along the way and the run's GPU-hang evidence at the end.
set -uo pipefail
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:threshold-stereo} QUIVER_LAB_OWNER=${QUIVER_LAB_OWNER:-claude:threshold-stereo}
mod=$1 i=control-stereo
q() { quiver --machine rayblaster lab "$@"; }
q kill $i >/dev/null 2>&1
q fixture install $i game-start >/dev/null 2>&1
ssh -o BatchMode=yes rayblaster 'chmod -R u+w ~/Games/quiver-lab/instances/control-stereo/prefix/drive_c/users/steamuser/Documents/My\ Games/Control/Saves'
q launch $i --mod "$mod" --wait frame --timeout 400 | tail -1
for k in Return Return Escape Return; do q shot $i /dev/null --settle 5 >/dev/null 2>&1; q key $i $k >/dev/null; done
if ! q wait $i in-world --timeout 240 >/dev/null 2>&1; then   # the DLC promo panel came up late
  q key $i Escape >/dev/null; q shot $i /dev/null --settle 3 >/dev/null 2>&1; q key $i Return >/dev/null
  q wait $i in-world --timeout 240 >/dev/null 2>&1
fi
# TWEAKS="Name=value;Name=value": engine tweakables set through `stereo tweak` once in the world (bisecting)
if [ -n "${TWEAKS:-}" ]; then
  IFS=';' read -ra tw <<<"$TWEAKS"
  for t in "${tw[@]}"; do q call $i stereo tweak ${t%%=*} "${t#*=}" 2>&1 | tail -1; done
fi
st() { q call $i stereo status 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print('pairs', d['pairs'], 'worlds', d['worlds'], 'present', d['present'])" 2>/dev/null; }
last=""
for n in $(seq 1 90); do
  q wait $i frame --timeout 10 >/dev/null 2>&1; q shot $i /dev/null --settle 8 >/dev/null 2>&1
  s=$(st); echo "$(date +%T) $s"
  [ "$s" = "$last" ] && { echo "frozen"; break; }
  last=$s
  q where $i 2>/dev/null | grep -q '"control":true' && q where $i 2>/dev/null | grep -q '"active":false' && { echo "in control"; break; }
done
ssh -o BatchMode=yes rayblaster 'L=$(ls -dt ~/Games/quiver-lab/instances/control-stereo/lab/runs/r* | head -1); echo run $L; echo signaller $(grep -c signaller $L/game.log); grep "nav: timeline\|stall: no" $L/threshold.log | tail -6; journalctl -k --since "-15min" | grep "Xid.*Control" | tail -1'
