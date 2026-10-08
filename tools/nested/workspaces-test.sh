#!/bin/bash
# The bar's workspace pills against the real niri: they follow the compositor, and a click switches workspace.
#   HELIX_NESTED_SCRIPT=tools/nested/workspaces-test.sh tools/nested/run.sh ws tools/nested/tauri-fake-desktop.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.."  && pwd); LOG=$SCRATCH/program.log; FAILED=0
LAYOUT=$SCRATCH/state/layout.json
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_ws() { for _ in $(seq 1 "${2:-40}"); do grep 'WORKSPACES [0-9]' "$LOG" | tail -1 | grep -Eq "$1" && return 0; sleep 0.25; done; echo "TIMEOUT waiting for: $1 (newest: $(grep 'WORKSPACES [0-9]' "$LOG" | tail -1))" >&2; return 1; }
active() { niri msg --json workspaces | python3 -c "import json,sys; print([w['idx'] for w in json.load(sys.stdin) if w['is_focused']][0])"; }
for _ in $(seq 1 120); do grep -q "LIVE Sound" "$LOG" && break; sleep 0.25; done
sleep 2
# niri keeps a spare empty workspace after the last one that has a window, so the strip only has two once something is open.
TARGET_FILE=$SCRATCH/t.txt APP_ID=wsapp python3 "$ROOT/tools/nested/text-target.py" & WIN=$!
trap 'kill $WIN 2>/dev/null' EXIT
sleep 3
echo "--- 1. the pills follow niri: two workspaces, the first active"
wait_ws 'WORKSPACES 2 active=Some\(1\)' || fail "strip not shown"
has_id ws-0 && has_id ws-1 || fail "pills missing from the bar"
echo "--- 2. a click switches workspace"
click_id ws-1
wait_ws 'active=Some\(2\)' || fail "the pill did not follow the switch"
[ "$(active)" = 2 ] && echo "ok: niri is on workspace 2" || fail "niri is on $(active)"
echo "--- 3. switching with the keyboard (niri) moves the pill"
niri msg action focus-workspace 1; wait_ws 'active=Some\(1\)' || fail "pill did not follow niri"
click_id ws-0
[ "$(active)" = 1 ] && echo "ok: clicking the active pill stays put" || fail "moved to $(active)"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
