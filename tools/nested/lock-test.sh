#!/bin/bash
# The lock bind in a nested compositor: helix-panel lock must take the session lock, and a wrong password must not release it.
#   HELIX_NESTED_SCRIPT=tools/nested/lock-test.sh tools/nested/run.sh lock tools/nested/idle-program.sh
set -u
SCRATCH=$1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
"$ROOT/tools/helix-panel" lock
sleep 2
LOCKER=$(pgrep -x swaylock | head -1)
[ -n "$LOCKER" ] || fail "no locker is running"
grep -qi 'lock' "$SCRATCH/niri.log" && echo "niri log mentions the lock"
niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5
ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/locked.png"
wtype "not the password"; sleep 0.3; wtype -k Return; sleep 3
kill -0 "$LOCKER" 2>/dev/null || fail "a wrong password released the lock"
# A second request while locked must not start another locker (swaylock keeps a helper process for PAM, so compare counts).
before=$(pgrep -cx swaylock)
"$ROOT/tools/helix-panel" lock; sleep 1
after=$(pgrep -cx swaylock); [ "$after" -eq "$before" ] || fail "a second locker started ($before -> $after)"
[ "$FAILED" -eq 0 ] && echo "ALL ok" || { echo FAILED; exit 1; }
