#!/bin/bash
# Agents panel against the real helix-service, a stand-in agent, a real pointer and the Agent Surface confirmer:
# the bar button, the kill switch, revoking access, stopping what is waiting, and the history line.
#   HELIX_NESTED_SCRIPT=tools/nested/activity-test.sh tools/nested/run.sh activity tools/nested/tauri-shell.sh
set -u
SCRATCH=$1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log
CONTROL=$SCRATCH/state/surface-control
FAILED=0
probe() { python3 "$ROOT/tools/nested/agent-probe.py" "$@"; }
BG=""
cleanup() { [ -n "$BG" ] && kill -0 "$BG" 2>/dev/null && kill "$BG" 2>/dev/null; true; }
trap cleanup EXIT
SHELL_LAYOUT=$SCRATCH/state/layout.json
LAYOUT=$SHELL_LAYOUT
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_for() {
  for _ in $(seq 1 "${3:-80}"); do
    [ "$(grep -c "$1" "$LOG" 2>/dev/null || true)" -ge "$2" ] && return 0
    sleep 0.25
  done
  echo "TIMEOUT waiting for $2 x $1" >&2
  return 1
}
# The newest "ACTIVITY view" line must match the pattern.
wait_view() {
  for _ in $(seq 1 "${2:-80}"); do
    grep 'ACTIVITY view' "$LOG" 2>/dev/null | tail -1 | grep -Eq "$1" && return 0
    sleep 0.25
  done
  echo "TIMEOUT waiting for view: $1 (newest: $(grep 'ACTIVITY view' "$LOG" | tail -1))" >&2
  return 1
}
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.2; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
# Grants access to the stand-in agent through the real confirmer overlay with the pointer.
grant_with_pointer() {
  local opened armed
  opened=$(grep -c "CONFIRMER open" "$LOG" || true)
  armed=$(grep -c "CONFIRMER armed" "$LOG" || true)
  probe grant observe >"$SCRATCH/probe-grant.log" 2>&1 &
  BG=$!
  wait_for "CONFIRMER open" $((opened + 1)) 40 || return 1
  wait_for "CONFIRMER armed" $((armed + 1)) 40 || return 1
  rm -f "$SCRATCH/state/layout-confirmer.json.seen"
  LAYOUT=$SCRATCH/state/layout-confirmer.json
  for _ in $(seq 1 40); do has_id confirm-allow && break; sleep 0.25; done
  click_id confirm-allow
  LAYOUT=$SHELL_LAYOUT
  wait "$BG"; BG=""
  grep -q 'COMPLETED ok=True' "$SCRATCH/probe-grant.log" || { cat "$SCRATCH/probe-grant.log"; return 1; }
}
act() { probe act status 2>&1; }

wait_for "LIVE Sound" 1 120 || exit 1
wait_for "CONFIRMER registered" 1 80 || exit 1
PID=$(cat "$SCRATCH/state/shell.pid")

echo "--- 1. Activity is available once the shell is the registered desktop; nobody has access yet"
wait_view 'available=true enabled=true busy=false pending=0 grants=0' || exit 1
for _ in $(seq 1 40); do has_id agentsbtn && break; sleep 0.25; done
has_id agentsbtn || { echo "FAIL: the Agents button is missing from the bar"; cat "$LAYOUT" >&2; exit 1; }

echo "--- 2. A request waiting on a person makes the bar busy; granting it clears that"
grant_with_pointer || { echo "FAIL: pointer grant did not complete" >&2; exit 1; }
wait_view 'busy=false pending=0 grants=1' || fail "grant not reflected"
echo "ok: grant via pointer reflected"

echo "--- 3. An allowed action lands in the history"
out=$(act); echo "$out"
wait_view 'last=status/allowed' || fail "allowed action not in the history"

echo "--- 4. The Agents panel opens from the bar button"
click_id agentsbtn
for _ in $(seq 1 20); do grep -q '"panel": *"assistant"' "$LAYOUT" 2>/dev/null && break; sleep 0.25; done
grep -q '"panel": *"assistant"' "$LAYOUT" || fail "the Mobius agents view did not open"
for _ in $(seq 1 40); do has_id agent-toggle && has_id revoke-0 && break; sleep 0.25; done
has_id revoke-0 || fail "revoke-0 missing"
shot activity-panel
python3 "$ROOT/tools/nested/process_tree.py" "$PID" "agents-panel"

echo "--- 5. Kill switch: off refuses everything, on restores it"
click_id agent-toggle
wait_view 'enabled=false' || fail "kill switch did not turn off"
out=$(act); echo "$out"
echo "$out" | grep -q 'turned off' || fail "request was not refused while off"
wait_view 'last=status/denied/agent disabled' || fail "refusal not in the history"
shot activity-off
click_id agent-toggle
wait_view 'enabled=true' || fail "kill switch did not turn back on"
out=$(act); echo "$out"
echo "$out" | grep -q 'turned off' && fail "request refused after turning back on"
wait_view 'last=status/allowed' || fail "action not allowed again"
echo "ok: kill switch"

echo "--- 6. Revoke removes access"
click_id revoke-0
wait_view 'grants=0' || fail "grant not revoked"
out=$(act); echo "$out"
wait_view 'last=status/denied' || fail "request not denied after revoke"
echo "ok: revoke"

echo "--- 7. Stop cancels what is waiting without revoking anything"
opened=$(grep -c "CONFIRMER open" "$LOG" || true)
probe grant observe >"$SCRATCH/probe-stop.log" 2>&1 &
BG=$!
wait_for "CONFIRMER open" $((opened + 1)) 40 || exit 1
wait_view 'busy=true pending=1' || fail "pending request not shown"
echo agent-stop-all >>"$CONTROL"
wait_view 'busy=false pending=0' || fail "stop did not clear the queue"
wait "$BG"; BG=""
cat "$SCRATCH/probe-stop.log"
grep -q 'cancelled' "$SCRATCH/probe-stop.log" || fail "the agent was not told its request was cancelled"
wait_for "CONFIRMER destroyed" $((opened + 1)) 40 || fail "the dialog stayed up after Stop"
echo "ok: stop"

echo "--- 8. The shell claims its roles again when the service restarts"
OLD=$(cat "$SCRATCH/state/helix-service.pid")
kill "$OLD"
for _ in $(seq 1 40); do kill -0 "$OLD" 2>/dev/null || break; sleep 0.25; done
registered=$(grep -c "CONFIRMER registered" "$LOG" || true)
HELIX_RISK_POLICY="$ROOT/protocols/ipc/msg-risk.toml" PIPEWIRE_REMOTE=helix-nested-no-audio \
  "${HELIX_SERVICE_BIN:-$ROOT/target/fast/helix-desktop}" >>"$SCRATCH/state/helix-service.log" 2>&1 &
echo $! >"$SCRATCH/state/helix-service.pid"
wait_for "CONFIRMER service restarted" 1 80 || fail "the shell did not notice the new service"
wait_for "CONFIRMER registered" $((registered + 1)) 80 || fail "the shell did not register again"
wait_view 'available=true enabled=true busy=false pending=0 grants=0' || fail "activity did not come back"
grant_with_pointer || fail "an approval dialog did not work after the restart"
echo "ok: roles reclaimed"

if [ "$FAILED" -ne 0 ]; then echo FAILED; exit 1; fi
echo "ALL ok"
