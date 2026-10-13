#!/bin/bash
# The live-status pill in the bar: a stand-in media player makes music appear in it and clicking it plays or pauses; an agent waiting
# for approval takes over the pill; with nothing going on it is gone.
#   HELIX_NESTED_SCRIPT=tools/nested/island-test.sh tools/nested/run.sh island tools/nested/tauri-shell.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.."  && pwd); LOG=$SCRATCH/program.log; FAILED=0
LAYOUT=$SCRATCH/state/layout.json; export MPRIS_LOG=$SCRATCH/mpris.log MPRIS_CMD=$SCRATCH/mpris.cmd
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_island() { for _ in $(seq 1 "${2:-40}"); do grep '^ISLAND' "$LOG" | tail -1 | grep -q "$1" && return 0; sleep 0.25; done; echo "TIMEOUT waiting for island $1 (newest: $(grep '^ISLAND' "$LOG" | tail -1))" >&2; return 1; }
for _ in $(seq 1 120); do grep -q "LIVE Sound" "$LOG" && break; sleep 0.25; done
sleep 1
echo "--- 1. nothing going on: no pill"
has_id island && fail "the pill is shown with nothing going on" || echo "ok: no pill"

echo "--- 2. music appears in the pill, and clicking it pauses"
: > "$MPRIS_CMD"; rm -f "$MPRIS_LOG"
python3 "$ROOT/tools/nested/mpris-app.py" 2>/dev/null & PLAYER=$!
trap 'kill $PLAYER 2>/dev/null' EXIT
wait_island Media || fail "music did not appear"
for _ in $(seq 1 20); do has_id island && break; sleep 0.25; done
has_id island && echo "ok: the pill is in the bar" || fail "no pill in the layout"
click_id island; for _ in $(seq 1 20); do grep -q PlayPause "$MPRIS_LOG" 2>/dev/null && break; sleep 0.25; done
grep -q PlayPause "$MPRIS_LOG" && echo "ok: the click paused the player" || fail "click did not reach the player"
wait_island None || fail "the pill stayed after pausing"
echo "status Playing" >> "$MPRIS_CMD"; wait_island Media || fail "playing again did not bring it back"

echo "--- 3. an agent waiting for approval takes over the pill"
python3 "$ROOT/tools/nested/agent-probe.py" grant observe >"$SCRATCH/grant.log" 2>&1 & PROBE=$!
wait_island Approval 60 && echo "ok: waiting for you" || fail "a pending approval did not show"
sleep 1.5; wtype -k Escape    # the confirmation dialog denies
wait "$PROBE" 2>/dev/null
wait_island Media && echo "ok: back to the music once decided" || fail "the pill did not return to the music"

echo "--- 4. the player goes: the pill goes"
echo quit >> "$MPRIS_CMD"; wait_island None 40 && echo "ok: gone" || fail "the pill stayed after the player quit"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
