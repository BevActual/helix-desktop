#!/bin/bash
# HELIX_NESTED_SCRIPT: Agent1 confirmer Overlay with pointer and Escape.
#   HELIX_NESTED_SCRIPT=tools/nested/confirmer-test.sh tools/nested/run.sh confirmer tools/nested/tauri-shell.sh
set -u
SCRATCH=$1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log
export LAYOUT=$SCRATCH/state/layout-confirmer.json
FAILED=0
CLIENT=""
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
  if [ -n "$CLIENT" ] && kill -0 "$CLIENT" 2>/dev/null; then
    kill "$CLIENT" 2>/dev/null || true
  fi
  if [ -f "$SCRATCH/state/helix-service.pid" ]; then
    kill "$(cat "$SCRATCH/state/helix-service.pid")" 2>/dev/null || true
  fi
}
trap cleanup EXIT
source "$ROOT/tools/nested/ui-lib.sh"
shot() {
  niri msg action screenshot-screen >/dev/null 2>&1
  sleep 1.2
  ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"
}
# Must not run inside $(...): the background job has to be a child of this shell for wait.
start_request() {
  python3 "$ROOT/tools/nested/confirmer-request.py" "$@" >"$SCRATCH/confirmer-client.log" 2>&1 &
  CLIENT=$!
}
expect_denied() {
  wait "$CLIENT"
  local status=$?
  CLIENT=""
  cat "$SCRATCH/confirmer-client.log"
  if [ "$status" -ne 0 ] || ! grep -q 'COMPLETED ok=False' "$SCRATCH/confirmer-client.log"; then
    echo "FAIL: expected denial" >&2
    FAILED=1
    return 1
  fi
  return 0
}
expect_granted() {
  wait "$CLIENT"
  local status=$?
  CLIENT=""
  cat "$SCRATCH/confirmer-client.log"
  # confirmer-request exits 1 when ok=True
  if [ "$status" -ne 1 ] || ! grep -q 'COMPLETED ok=True' "$SCRATCH/confirmer-client.log"; then
    echo "FAIL: expected grant" >&2
    FAILED=1
    return 1
  fi
  return 0
}

wait_for "LIVE Sound" 1 120 || exit 1
wait_for "CONFIRMER registered" 1 80 || exit 1
PID=$(cat "$SCRATCH/state/shell.pid")

echo "--- 1. Escape denies (Enter never approves)"
start_request observe act
wait_for "CONFIRMER open" 1 40 || exit 1
sleep 0.5
python3 "$ROOT/tools/nested/process_tree.py" "$PID" "confirmer-open"
shot confirmer-open
wtype -k Return
sleep 0.4
wtype -k Escape
wait_for "CONFIRMER destroyed" 1 40 || exit 1
expect_denied && echo "ok: Escape denied"

echo "--- 2. Pointer Deny"
start_request observe act
wait_for "CONFIRMER open" 2 40 || exit 1
wait_for "CONFIRMER ready-replay" 2 40 || { echo "FAIL: confirmer page never ready"; FAILED=1; }
# Wait until the named control is reported (layout file appears after ready-replay).
for _ in $(seq 1 40); do has_id confirm-deny && break; sleep 0.25; done
has_id confirm-deny || { echo "FAIL: confirm-deny missing from layout"; cat "$LAYOUT" >&2; exit 1; }
click_id confirm-deny
wait_for "CONFIRMER destroyed" 2 40 || exit 1
expect_denied && echo "ok: pointer Deny"

echo "--- 3. Pointer Allow after arming (grant, confirm stays unchecked)"
# Arming before 700 ms is covered in helix-shell-core unit tests; Enter-never is step 1.
# Here: wait until armed, Allow with the pointer, confirm tier stays opt-in.
ARMED_BEFORE=$(grep -c "CONFIRMER armed" "$LOG" 2>/dev/null || true)
rm -f "$LAYOUT"
start_request observe confirm
wait_for "CONFIRMER open" 3 40 || exit 1
wait_for "CONFIRMER ready-replay" 3 40 || exit 1
wait_for "CONFIRMER armed" $((ARMED_BEFORE + 1)) 20 || { echo "FAIL: never armed"; exit 1; }
for _ in $(seq 1 40); do has_id confirm-allow && break; sleep 0.25; done
has_id confirm-tier-observe || { echo "FAIL: grant tiers missing"; cat "$LAYOUT" >&2; FAILED=1; }
has_id confirm-tier-confirm || { echo "FAIL: confirm tier missing"; FAILED=1; }
click_id confirm-allow
wait_for "CONFIRMER destroyed" 3 40 || exit 1
expect_granted && echo "ok: Allow granted observe only (confirm unchecked)"
grep -q '"granted": \["observe"\]' "$SCRATCH/confirmer-client.log" || {
  echo "FAIL: confirm tier was granted without being checked" >&2
  FAILED=1
}

echo "--- 4. ConfirmationEnded drops a stale dialog"
start_request --cancel-after 1.5 observe act
wait_for "CONFIRMER open" 4 40 || exit 1
wait_for "CONFIRMER destroyed" 4 40 || exit 1
expect_denied && echo "ok: Cancel ended the dialog"
shot confirmer-closed
python3 "$ROOT/tools/nested/process_tree.py" "$PID" "after-confirmer"

if [ "$FAILED" -ne 0 ]; then
  echo "FAILED"
  exit 1
fi
echo "ALL ok"
