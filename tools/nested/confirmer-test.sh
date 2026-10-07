#!/bin/bash
# HELIX_NESTED_SCRIPT: Agent1 confirmer Overlay. A second bus peer requests access; Escape denies.
#   HELIX_NESTED_SCRIPT=tools/nested/confirmer-test.sh tools/nested/run.sh confirmer tools/nested/tauri-shell.sh
set -u
SCRATCH=$1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log
wait_for() {
  local needle=$1 need=$2
  for _ in $(seq 1 "${3:-80}"); do
    local n
    n=$(grep -c "$needle" "$LOG" 2>/dev/null || true)
    [ "$n" -ge "$need" ] && return 0
    sleep 0.25
  done
  echo "TIMEOUT waiting for $need x $needle" >&2
  return 1
}
cleanup() {
  if [ -f "$SCRATCH/state/helix-service.pid" ]; then
    kill "$(cat "$SCRATCH/state/helix-service.pid")" 2>/dev/null || true
  fi
}
trap cleanup EXIT

wait_for "LIVE Sound" 1 120 || exit 1
wait_for "CONFIRMER registered" 1 80 || exit 1
echo "--- 1. RequestAccess opens the confirmer; Escape denies"
python3 "$ROOT/tools/nested/confirmer-request.py" observe act >"$SCRATCH/confirmer-client.log" 2>&1 &
CLIENT=$!
wait_for "CONFIRMER open" 1 40 || { kill "$CLIENT" 2>/dev/null; exit 1; }
sleep 0.5
niri msg action screenshot-screen >/dev/null 2>&1
sleep 1.2
ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/confirmer-open.png"
# Enter must not approve: press it before Allow would arm, then Escape to deny.
wtype -k Return
sleep 0.4
wtype -k Escape
wait_for "CONFIRMER destroyed" 1 40 || { kill "$CLIENT" 2>/dev/null; exit 1; }
wait "$CLIENT"
CLIENT_STATUS=$?
echo "--- client log ---"
cat "$SCRATCH/confirmer-client.log"
if [ "$CLIENT_STATUS" -ne 0 ]; then
  echo "FAIL: client expected a denial (exit 0), got $CLIENT_STATUS" >&2
  exit 1
fi
grep -q 'COMPLETED ok=False' "$SCRATCH/confirmer-client.log" || {
  echo "FAIL: Completed was not a denial" >&2
  exit 1
}
echo "ok: Escape denied the grant; confirmer destroyed"
PID=$(cat "$SCRATCH/state/shell.pid")
python3 "$ROOT/tools/nested/process_tree.py" "$PID" "after-confirmer"
