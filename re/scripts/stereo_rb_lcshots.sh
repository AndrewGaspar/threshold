#!/usr/bin/env bash
# stereo_rb_lcshots.sh OUTDIR [locations...]: on rayblaster's control-stereo (in the world, sbs): for each
# location (or `restart` for a checkpoint restart, whose video is skipped), change level and take
# screenshots 1, 4 and 10 s after the game is back in the world, with the pair counters, to show
# whether 3D runs through the level change's streaming tail and both eyes are right afterwards.
set -uo pipefail
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:threshold-stereo} QUIVER_LAB_OWNER=${QUIVER_LAB_OWNER:-claude:threshold-stereo}
out=$1; shift; i=control-stereo; mkdir -p "$out"
q() { quiver --machine rayblaster lab "$@"; }
st() { q call $i stereo status 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); print('pairs', d['pairs'], 'worlds', d['worlds'], 'mono_fb', d.get('mono_fallback'), 'load_mono', d.get('load_mono'), 'rt_skip', d.get('rt_upd_skipped'), 'left_builds', d.get('left_builds'))" 2>/dev/null; }
for loc in "${@:-cafeteria dead-letters restart}"; do
  if [ "$loc" = restart ]; then
    q call $i restart >/dev/null 2>&1
    for n in $(seq 1 20); do q wait $i frame --timeout 5 >/dev/null 2>&1; q key $i space >/dev/null 2>&1; q shot $i /dev/null --settle 3 >/dev/null 2>&1; q where $i 2>/dev/null | grep -q '"control":true' && break; done
  else
    q teleport $i $loc --timeout 150 2>&1 | tail -1 | cut -c1-50
  fi
  for t in 1 4 10; do
    f=$(q shot $i --settle $t 2>&1 | tail -1); cp "$f" "$out/$loc-$t.png" 2>/dev/null
    echo "$(date +%T) $loc +${t}s: $(st)"
  done
done
