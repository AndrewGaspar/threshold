#!/usr/bin/env bash
# stereo_rb_soak.sh [SECS]: 3D soak on rayblaster's control-stereo, which must already be in the world in
# sbs. Cycles teleports (nav verbs) with ~20 s of rendering after each, for SECS (default 900). (A
# checkpoint restart lands in a pre-rendered video with no world renders, so it is not in the cycle.) One line per step; stops on a frozen pair count or a teleport error,
# then prints the run's signaller-timeout count, stalls and Xids.
set -uo pipefail
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:threshold-stereo} QUIVER_LAB_OWNER=${QUIVER_LAB_OWNER:-claude:threshold-stereo}
secs=${1:-900} i=control-stereo
q() { quiver --machine rayblaster lab "$@"; }
st() { q call $i stereo status 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print('pairs', d['pairs'], 'present', d['present'], 'changes', d.get('level_changes'), 'as_skip', d.get('as_skipped'), 'refit_skip', d.get('refit_skipped'))" 2>/dev/null; }
pairs() { st | awk '{print $2}'; }
play() { for n in 1 2 3; do q wait $i frame --timeout 6 >/dev/null 2>&1; q shot $i /dev/null --settle 6 >/dev/null 2>&1; done; }
# a video or cinematic first (no world renders): skip it until pairs advance
for n in $(seq 1 30); do a=$(pairs); q key $i space >/dev/null; play; [ "$(pairs)" != "$a" ] && break; done
t0=$(date +%s) steps=0 fail=""
echo "start $(date +%T): $(st)"
while [ $(( $(date +%s) - t0 )) -lt "$secs" ] && [ -z "$fail" ]; do
  for loc in mail-room executive-corridor cafeteria motel dead-letters entry-lobby; do
    a=$(pairs)
    if [ $loc = restart ]; then q call $i restart >/dev/null 2>&1; r=$(q wait $i in-world --timeout 240 2>&1 | tail -1 | cut -c1-40)
    else r=$(q teleport $i $loc --timeout 150 2>&1 | tail -1 | cut -c1-60); fi
    play; b=$(pairs); steps=$((steps + 1))
    echo "$(date +%T) $loc: $r | $(st)"
    if echo "$r" | grep -qi error || [ -z "$b" ] || [ "$a" = "$b" ]; then fail="$loc"; echo "FAILED at $loc"; break; fi
    [ $(( $(date +%s) - t0 )) -ge "$secs" ] && break
  done
done
echo "steps $steps in $(( $(date +%s) - t0 )) s${fail:+, failed at $fail}"
ssh -o BatchMode=yes rayblaster 'L=$(ls -dt ~/Games/quiver-lab/instances/control-stereo/lab/runs/r* | head -1); echo run $L; echo signaller $(grep -c signaller $L/game.log); grep "stall: no" $L/threshold.log | head -3; journalctl -k --since "-20min" | grep "Xid.*Control" | tail -2'
