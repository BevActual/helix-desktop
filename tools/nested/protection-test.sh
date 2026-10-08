#!/bin/bash
# What the shell does about the approval dialog's protection. A client holding the confirm tier asks to run a confirm-tier action.
#   protected compositor:   the dialog opens, is found to be a trusted surface, and Allow arms
#   unprotected compositor: the action is refused without being shown (nothing to click) and the client is told it was denied
#   tools/nested/trusted-run.sh protected protection-test.sh   |   tools/nested/trusted-run.sh stock protection-test.sh
set -u
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log
export LAYOUT=$SCRATCH/state/layout-confirmer.json
EXPECT=${TRUSTED_EXPECT:-stock}; FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
count() { grep -c "$1" "$LOG" 2>/dev/null || true; }
wait_for() { for _ in $(seq 1 "${3:-80}"); do [ "$(count "$1")" -ge "$2" ] && return 0; sleep 0.25; done; return 1; }
wait_for "LIVE Sound" 1 120 || exit 1; wait_for "CONFIRMER registered" 1 80 || exit 1
"$HELIX_PROBE_PYTHON" "$ROOT/tools/nested/agent-probe.py" act app-launch Files >"$SCRATCH/probe.out" 2>&1 &
PROBE=$!
if [ "$EXPECT" = protected ]; then
  wait_for "CONFIRMER protection=protected" 1 40 && echo "ok: the dialog was found to be a trusted surface" || fail "protection was not found: $(grep CONFIRMER "$LOG" | tail -3)"
  wait_for "CONFIRMER armed" 1 20 && echo "ok: Allow arms once protected" || fail "never armed"
  has_id confirm-protection && python3 -c "
import json; e=json.load(open('$LAYOUT'))['elements']
print('banner shown' if 'confirm-protection' in e else 'no banner')" | grep -q "no banner" && echo "ok: no warning is shown when protected" || true
  kill -0 "$PROBE" 2>/dev/null && echo "ok: the action is waiting for a decision" || fail "the action did not wait: $(cat "$SCRATCH/probe.out")"
  wtype -k Escape; sleep 1
else
  wait_for "CONFIRMER protection=unprotected" 1 40 && echo "ok: the compositor was found not to protect the dialog" || fail "unprotected not found: $(grep CONFIRMER "$LOG" | tail -3)"
  wait "$PROBE"; grep -q "COMPLETED ok=False" "$SCRATCH/probe.out" && echo "ok: the action was refused" || fail "the action was not refused: $(cat "$SCRATCH/probe.out")"
  wait_for "CONFIRMER destroyed" 1 20 && echo "ok: the dialog closed on its own" || fail "the dialog stayed"
  grep -q "CONFIRMER armed" "$LOG" && fail "Allow was armed on an unprotected compositor" || echo "ok: Allow never armed"
fi
kill "$PROBE" 2>/dev/null
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
