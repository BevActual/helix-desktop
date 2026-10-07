#!/bin/bash
# HELIX_NESTED_SCRIPT: the assistant inside the Tauri shell, end to end with the real model.
#   HELIX_MODEL_DIR=<models> HELIX_NESTED_ASSISTANT=<build>/helix-assistant PATH=<build>:$PATH \
#     HELIX_NESTED_SCRIPT=tools/nested/assistant-flow-test.sh tools/nested/run.sh flow tools/nested/tauri-shell.sh
# Types a request, waits for the confirmation, checks Enter does not confirm, presses Do it, and waits for the result.
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control; PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
wait_for() { for _ in $(seq 1 "${3:-240}"); do grep -q "$1" "$LOG" 2>/dev/null && return 0; sleep 0.5; done; echo "TIMEOUT waiting for: $1"; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
count() { grep -c "$1" "$LOG"; }
wait_for "LIVE Sound" 1 60 || exit 1
echo open >> "$CONTROL"; wait_for "PANEL open" 1 40 || exit 1
sleep 1; wtype "Make the screen dimmer."; sleep 0.4; wtype -k Return
echo "--- request sent"; wait_for "ASSISTANT thinking" 1 20 || exit 1
wait_for "ASSISTANT confirm" 1 360 || { shot flow-timeout; exit 1; }
echo "--- confirmation reached"; shot flow-confirm
BEFORE=$(count "ASSISTANT acting")
wtype -k Return; sleep 1.2; wtype -k Return; sleep 1.2
echo "enter presses started an action: $(( $(count 'ASSISTANT acting') - BEFORE ))"
sleep 0.8
# Do it is the right-hand button of the confirmation row (found from a screenshot of this layout: about 62.7% across, 50.7% down).
"$PT" 0.627 0.507 click; sleep 0.5
wait_for "ASSISTANT acting" 1 20 && echo "--- Do it started the action"
wait_for "ASSISTANT result" 1 60 && echo "--- result shown"
shot flow-result
grep -E "ASSISTANT" "$LOG" | tr '\n' ' '; echo
