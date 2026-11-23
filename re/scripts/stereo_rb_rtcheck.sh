#!/usr/bin/env bash
# stereo_rb_rtcheck.sh MODDIR [SOAK_SECS]: the RT-fault check (docs/stereo.md "Level-change hang"): Continue, the
# opening cinematic untouched, IDLE (default 8) minutes in the world with Xid/pair checks, then a teleport soak.
# launch, cinematic to control, idle until 10 min of control (Xid/pairs checks), then a 600 s teleport soak

export QUIVER_SOURCE=agent:threshold-stereo QUIVER_LAB_OWNER=claude:threshold-stereo
mod=$1; since=$(date '+%Y-%m-%d %H:%M:%S')
cd "$(dirname "$0")/../.."
re/scripts/stereo_rb_cinematic.sh $mod | grep -v sending | tail -4
last=""
for n in $(seq 1 ${IDLE:-8}); do
  p=$(quiver --machine rayblaster lab call control-stereo stereo status 2>/dev/null | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["pairs"], "new", d.get("as_new"), "built", d.get("as_new_built"))' 2>/dev/null)
  x=$(ssh -o BatchMode=yes rayblaster "journalctl -k --since '$since' | grep -c Xid")
  echo "$(date +%T) idle pairs $p xid $x"
  [ "$x" != "0" ] && { echo FAULT; exit 1; }
  [ -n "$last" ] && [ "$p" = "$last" ] && { echo FROZEN; exit 1; }
  last=$p; sleep 60
done
re/scripts/stereo_rb_soak.sh ${2:-600} | grep -v sending
ssh -o BatchMode=yes rayblaster "journalctl -k --since '$since' | grep Xid | tail -2"
