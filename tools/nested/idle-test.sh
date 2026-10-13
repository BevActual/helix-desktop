#!/bin/bash
# helix-idle against the real compositor: nothing touched, so it locks, then turns the screen off; a touch turns it back on and the lock
# does not repeat until the next idle period. Real input comes from the virtual pointer.
#   HELIX_NESTED_SCRIPT=tools/nested/idle-test.sh tools/nested/run.sh idle tools/nested/idle-nested.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.."  && pwd); LOG=$SCRATCH/program.log; HOOKS=$SCRATCH/state/idle-hooks.log; FAILED=0
PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
waitfor() { for _ in $(seq 1 "${2:-120}"); do grep -qx "$1" "$HOOKS" 2>/dev/null && return 0; sleep 0.5; done; return 1; }
grep -q "IDLE watching" "$LOG" || sleep 2
echo "--- 1. untouched: the lock comes first, the screen off after it"
waitfor lock 60 && echo "ok: locked" || fail "no lock while idle"
[ "$(grep -c -x off "$HOOKS")" = 0 ] && echo "ok: screen still on at the moment of locking" || fail "the screen went off before the lock"
waitfor off 40 && echo "ok: screen off" || fail "the screen never went off"
echo "--- 2. a touch turns the screen back on"
"$PT" 0.5 0.5
waitfor on 20 && echo "ok: screen on again" || fail "the screen did not come back on a touch"
echo "--- 3. the lock does not repeat straight away"
sleep 4
[ "$(grep -c -x lock "$HOOKS")" = 1 ] && echo "ok: locked once" || fail "locked $(grep -c -x lock "$HOOKS") times"
order=$(tr '\n' ' ' < "$HOOKS"); echo "hook order: $order"
[ "$order" = "lock off on " ] && echo "ok: lock, off, on" || fail "unexpected order: $order"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
