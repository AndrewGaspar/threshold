#!/usr/bin/env bash
# stereo_rb_teleports.sh MODDIR [hdr]: (level-change hang test) launch on rayblaster, drive into the world, 6 teleports; prints a line per step.
cd /home/ajg/code/threshold
export QUIVER_SOURCE=agent:threshold-stereo QUIVER_LAB_OWNER=claude:threshold-stereo
R=tools/lab/rb; mod=$1; hdr=${2:-}
$R quiver lab kill control-stereo >/dev/null 2>&1
$R quiver lab fixture install control-stereo game-start >/dev/null 2>&1
ssh -o BatchMode=yes rayblaster 'chmod -R u+w ~/Games/quiver-lab/instances/control-stereo/prefix/drive_c/users/steamuser/Documents/My\ Games/Control/Saves'
$R quiver lab launch control-stereo --mod $mod --wait frame --timeout 300 | tail -1
$R quiver lab shot control-stereo /dev/null --settle 6 >/dev/null
[ -n "$hdr" ] && $R quiver lab click control-stereo 0.108 0.786 >/dev/null 2>&1
for k in Return Return Escape Return; do $R quiver lab shot control-stereo /dev/null --settle 5 >/dev/null; $R quiver lab key control-stereo $k; done
$R quiver lab wait control-stereo in-world --timeout 200 >/dev/null 2>&1 || { $R quiver lab key control-stereo Escape; $R quiver lab shot control-stereo /dev/null --settle 3 >/dev/null; $R quiver lab key control-stereo Return; $R quiver lab wait control-stereo in-world --timeout 200 >/dev/null 2>&1; }
$R quiver lab key control-stereo space --hold 1500 >/dev/null
for i in $(seq 1 12); do $R quiver lab key control-stereo space >/dev/null; $R quiver lab shot control-stereo /dev/null --settle 6 >/dev/null; $R quiver lab where control-stereo 2>/dev/null | grep -q '"control":true' && break; done
st() { $R quiver lab call control-stereo stereo status 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['pairs'], d['present'], d.get('hud_wide'))" 2>/dev/null; }
echo "start: $(st)"
for loc in mail-room executive-corridor cafeteria motel dead-letters entry-lobby; do
  r=$($R quiver lab teleport control-stereo $loc --timeout 120 2>&1 | tail -1 | cut -c1-50)
  echo "$loc: $r | $(st)"; echo "$r" | grep -q Error && break
done
$R bash -c 'L=$(quiver lab logs control-stereo | head -1 | awk "{print \$NF}"); echo run $L; echo signaller $(grep -c signaller $L/game.log); grep "RT async\|stall: no" $L/threshold.log | head -4'
