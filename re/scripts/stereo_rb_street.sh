#!/usr/bin/env bash
# stereo_rb_street.sh MODDIR: rayblaster's control-stereo from the game-start fixture into the lobby,
# camera turned to the street (yaw ~0), Jesse walked to the glass, frozen in Photo Mode with its UI
# hidden: the scene of the car-glass and blowing-leaves checks (docs/stereo.md).
set -uo pipefail
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:threshold-stereo} QUIVER_LAB_OWNER=${QUIVER_LAB_OWNER:-claude:threshold-stereo}
mod=$1 i=control-stereo
q() { quiver --machine rayblaster lab "$@"; }
q kill $i >/dev/null 2>&1
q fixture install $i game-start >/dev/null 2>&1
ssh -o BatchMode=yes rayblaster 'chmod -R u+w ~/Games/quiver-lab/instances/control-stereo/prefix/drive_c/users/steamuser/Documents/My\ Games/Control/Saves'
q launch $i --mod "$mod" --wait frame --timeout 400 | tail -1
for k in Return Return Escape Return; do q shot $i /dev/null --settle 5 >/dev/null 2>&1; q key $i $k >/dev/null; done
if ! q wait $i in-world --timeout 240 >/dev/null 2>&1; then
  q key $i Escape >/dev/null; q shot $i /dev/null --settle 3 >/dev/null 2>&1; q key $i Return >/dev/null
  q wait $i in-world --timeout 240 >/dev/null 2>&1
fi
q key $i space --hold 1500 >/dev/null
for n in $(seq 1 40); do q where $i 2>/dev/null | grep -q '"control":true' && break; q key $i space >/dev/null; q shot $i /dev/null --settle 8 >/dev/null 2>&1; done
q shot $i /dev/null --settle 5 >/dev/null 2>&1
yaw() { q where $i 2>/dev/null | grep -o '"camera":{"yaw":[-0-9.]*' | grep -o '[-0-9.]*$'; }
for n in $(seq 1 12); do   # ~300 px of mouse-look per 50 degrees here
  y=$(yaw); [ -z "$y" ] && break
  dx=$(python3 -I -c "y=float('$y'); d=-y if abs(y)<180 else 0; d=(d+180)%360-180; print(int(round(d*6)))")
  [ "${dx#-}" -lt 6 ] && break
  [ "$dx" -gt 300 ] && dx=300; [ "$dx" -lt -300 ] && dx=-300
  q mouse $i $dx 0 --steps 10 >/dev/null 2>&1; q shot $i /dev/null --settle 1 >/dev/null 2>&1
done
echo "yaw $(yaw)"
q key $i w --hold 2500 >/dev/null 2>&1; q shot $i /dev/null --settle 3 >/dev/null 2>&1
for n in $(seq 1 6); do
  q key $i Escape >/dev/null; q shot $i /dev/null --settle 3 >/dev/null 2>&1
  q where $i 2>/dev/null | grep -q '"menu":true' && break
  q shot $i /dev/null --settle 4 >/dev/null 2>&1
done
q key $i Down Down Return >/dev/null; q shot $i /dev/null --settle 3 >/dev/null 2>&1; q key $i t >/dev/null
q shot $i --settle 3 2>&1 | tail -1
