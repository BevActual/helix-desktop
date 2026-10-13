#!/bin/bash
# A magnified icon returns to its size when the pointer leaves the dock (moving away quickly, onto another surface).
#   HELIX_NESTED_SCRIPT=tools/nested/dock-magnify-test.sh tools/nested/run.sh dock-magnify tools/nested/dock-nested.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.."  && pwd); LOG=$SCRATCH/program.log; FAILED=0
LAYOUT=$SCRATCH/state/layout.json
PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
for _ in $(seq 1 120); do grep -q "LIVE Sound" "$LOG" && break; sleep 0.25; done; sleep 2
width() { python3 - "$LAYOUT" <<'PY'
import json, sys
e = json.load(open(sys.argv[1]))["elements"].get("dock-item-0")
print(round(e["w"]) if e else 0)
PY
}
"$PT" 0.5 0.4; sleep 0.6; "$PT" 0.5 0.999; sleep 1.4
pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" dock-item-0 0.5)
read fx fy <<<"$pos"
for d in -0.01 0.0 0.01 0.0; do "$PT" "$(python3 -c "print($fx + $d)")" $fy; sleep 0.15; done; sleep 0.8
BIG=$(width); echo "under the pointer: $BIG px"
[ "$BIG" -gt 52 ] && echo "ok: the icon grew" || fail "the icon did not grow ($BIG)"
"$PT" 0.5 0.2; sleep 2.5      # straight off the dock, onto the middle of the screen
SMALL=$(width); echo "after leaving: $SMALL px"
[ "$SMALL" -le 52 ] && echo "ok: back to its size" || fail "still magnified after the pointer left ($SMALL)"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
