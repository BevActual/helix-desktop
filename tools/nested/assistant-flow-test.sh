#!/bin/bash
# HELIX_NESTED_SCRIPT: Agent1 confirmer Overlay (required) and optional assistant panel flow.
#   Agent1 confirmer (always):
#     HELIX_NESTED_SCRIPT=tools/nested/assistant-flow-test.sh tools/nested/run.sh flow tools/nested/tauri-shell.sh
#   Plus real-model assistant Do it (optional):
#     HELIX_MODEL_DIR=<models> HELIX_NESTED_ASSISTANT=<build>/helix-assistant PATH=<build>:$PATH \
#       HELIX_NESTED_SCRIPT=tools/nested/assistant-flow-test.sh tools/nested/run.sh flow tools/nested/tauri-shell.sh
set -u
SCRATCH=$1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log

# --- Phase 1: Agent1 confirmer Overlay against the real service (exact Completed results) ---
bash "$ROOT/tools/nested/confirmer-test.sh" "$SCRATCH" || exit 1

# --- Phase 2 (optional): assistant panel + real model ---
if [ -z "${HELIX_NESTED_ASSISTANT:-}" ] || [ -z "${HELIX_MODEL_DIR:-}" ]; then
  echo "assistant panel flow skipped (set HELIX_NESTED_ASSISTANT and HELIX_MODEL_DIR to run it)"
  exit 0
fi

CONTROL=$SCRATCH/state/surface-control
PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
wait_for() { for _ in $(seq 1 "${3:-240}"); do grep -q "$1" "$LOG" 2>/dev/null && return 0; sleep 0.5; done; echo "TIMEOUT waiting for: $1"; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
count() { grep -c "$1" "$LOG"; }

echo "--- assistant panel flow"
echo open >> "$CONTROL"; wait_for "PANEL open" 1 40 || exit 1
sleep 1; wtype "Make the screen dimmer."; sleep 0.4; wtype -k Return
echo "--- request sent"; wait_for "ASSISTANT thinking" 1 20 || exit 1
wait_for "ASSISTANT confirm" 1 360 || { shot flow-timeout; exit 1; }
echo "--- confirmation reached"; shot flow-confirm
BEFORE=$(count "ASSISTANT acting")
wtype -k Return; sleep 1.2; wtype -k Return; sleep 1.2
echo "enter presses started an action: $(( $(count 'ASSISTANT acting') - BEFORE ))"
sleep 0.8
"$PT" 0.627 0.507 click; sleep 0.5
wait_for "ASSISTANT acting" 1 20 && echo "--- Do it started the action"
wait_for "ASSISTANT result" 1 60 && echo "--- result shown"
shot flow-result
grep -E "ASSISTANT" "$LOG" | tr '\n' ' '; echo
