#!/usr/bin/env bash
# stereo_rb_fg_levels.sh OUT: level changes with 3D + frame generation on rayblaster's control-sfg, which
# must already be in the world in sbs with [fg] enabled (tools/fg/launch). 4x: 6 teleports, then a
# checkpoint restart; then 2x: 2 teleports. After each change: a grab of the first jobs (tools/fg/stereo-grab,
# per-eye check), a screenshot, and the stereo / fg counters. Then the run's CreateFeature count,
# signaller_worker timeouts and Xids.
set -uo pipefail
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:stereo-fg} QUIVER_LAB_OWNER=${QUIVER_LAB_OWNER:-claude:stereo-fg}
out=$1 i=control-sfg
here=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$out"
q() { quiver --machine rayblaster lab "$@"; }
st() {
  s=$(q call $i stereo status 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print('pairs', d['pairs'], 'changes', d.get('level_changes'))" 2>/dev/null)
  f=$(q call $i fg status 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); p=d['perf'] or {}; print('feat', d['feature'], d['feature_right'], 'per_eye', d['frames_per_eye'], 'mult', p.get('multiplier'), 'fail', p.get('fail'), 'drop', p.get('dropped'))" 2>/dev/null)
  echo "$s | $f"
}
gen() {  # per-eye summary of a grab: max same-eye error, min other-eye error, max disparity |diff| median
  python3 -I -c "
import json,sys
r=json.load(open('$out/$1.json'))
if not r: print('no triplet'); sys.exit()
print('G %d: same-eye err max %.2f, other-eye err min %.2f, |disp G-real| median max %.2f (baseline %.2f)' % (len(r),
  max(max(x['err_same_L'],x['err_same_R']) for x in r), min(min(x['err_other_L'],x['err_other_R']) for x in r),
  max(x.get('disp_absdiff_median',0) for x in r), max(x.get('disp_real_vs_prev_median',0) for x in r)))" 2>&1
}
step() {  # name
  q wait $i frame --timeout 10 >/dev/null 2>&1
  "$here/tools/fg/stereo-grab" $i "$out" "$1" 2 0 >/dev/null 2>&1
  q shot $i "$out/$1.png" --settle 3 >/dev/null 2>&1
  echo "$(date +%T) $1: $(st) | $(gen "$1")"
}
echo "$(date +%T) start: $(st)"
for loc in mail-room executive-corridor cafeteria motel dead-letters entry-lobby; do
  r=$(q teleport $i $loc --timeout 150 2>&1 | tail -1 | cut -c1-60)
  echo "  teleport $loc: $r"; step "x4-$loc"
done
q call $i restart >/dev/null 2>&1; q wait $i in-world --timeout 240 >/dev/null 2>&1
# the restart lands in a pre-rendered video: skip until pairs advance again
a=$(st | awk '{print $2}')
for n in $(seq 1 30); do q key $i space >/dev/null; q shot $i /dev/null --settle 6 >/dev/null 2>&1; [ "$(st | awk '{print $2}')" != "$a" ] && break; done
q call $i skip for=60 >/dev/null 2>&1
for n in $(seq 1 30); do q where $i 2>/dev/null | grep -q '"control":true' && break; q shot $i /dev/null --settle 6 >/dev/null 2>&1; done
step "x4-restart"
q set $i "install:threshold.ini:[fg]count=2" --wait >/dev/null 2>&1
for loc in cafeteria entry-lobby; do
  r=$(q teleport $i $loc --timeout 150 2>&1 | tail -1 | cut -c1-60)
  echo "  teleport $loc: $r"; step "x2-$loc"
done
ssh -o BatchMode=yes rayblaster 'L=$(ls -dt ~/Games/quiver-lab/instances/control-sfg/lab/runs/r* | head -1); echo run $L; echo signaller $(grep -c signaller $L/game.log); echo CreateFeature FrameGeneration $(grep -c "CreateFeature(FrameGeneration" $L/threshold.log), released $(grep -c "DLSS-G feature 1 released" $L/threshold.log); grep -c "fg: Evaluate(" $L/threshold.log | sed "s/^/eval errors /"; grep -E "exception" $L/threshold.log | head -3; journalctl -k --since "-40min" | grep "Xid" | tail -3; echo xid-check-done'
