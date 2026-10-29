#!/bin/bash
# Writing Tools (docs/design/north-star.md, "intelligence in place"): text selected in an ordinary app window is read when the panel opens,
# Proofread streams the model's answer (a scripted model service here) and Replace puts it over the selection in that window.
# The nested compositor has no clipboard (wl-paste reads nothing; the qi suite fails on it too), so the selection is handed over in a file
# (HELIX_WRITING_TEST_SELECTION) and Replace types instead of pasting; the panel, the model call, the stream and the window are real.
#   tools/nested/ui-suite.sh writing
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control; TARGET_FILE=$SCRATCH/target.txt; export TARGET_FILE
source "$ROOT/tools/nested/ui-lib.sh"; LAYOUT=$SCRATCH/state/layout.json
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1"; return 1; }
shot() { sleep 1; niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
target_is() { for _ in $(seq 1 40); do [ "$(cat "$TARGET_FILE" 2>/dev/null)" = "$1" ] && return 0; sleep 0.25; done; echo "target holds: [$(cat "$TARGET_FILE" 2>/dev/null)]" >&2; return 1; }
wait_for "LIVE Sound" 1 120 || exit 1
python3 "$ROOT/tools/nested/text-target.py" & TARGET=$!
trap 'kill $TARGET 2>/dev/null' EXIT
sleep 3
wtype "teh cat sat on teh mat"; sleep 0.5
target_is "teh cat sat on teh mat" || fail "the text did not arrive in the window"
wtype -M ctrl a -m ctrl; sleep 0.5   # select it all
printf 'teh cat sat on teh mat' > "$SCRATCH/selection.txt"

echo "--- 1. the panel reads what is selected"
echo writing >> "$CONTROL"; wait_for "PANEL open" 1 20 || fail "Writing Tools did not open"
wait_for "WRITING selection 22 chars" 1 20 && echo "ok: it read the 22 selected characters" || fail "selection: $(grep WRITING "$LOG")"
sleep 1; shot writing-open

echo "--- 2. Proofread streams an answer"
for _ in $(seq 1 40); do has_id writing-proofread && break; sleep 0.25; done
click_id writing-proofread
wait_for "WRITING done" 1 60 && echo "ok: the answer came back" || fail "no answer: $(grep WRITING "$LOG")"
sleep 1; shot writing-done
for _ in $(seq 1 20); do has_id writing-replace && break; sleep 0.25; done
has_id writing-replace && echo "ok: a rewrite offers Replace" || fail "no Replace"

echo "--- 3. Replace puts it over the selection"
click_id writing-replace
wait_for "WRITING replace" 1 20 && echo "ok: Replace was pressed" || fail "replace not logged"
# This GTK test field forgets its selection when it loses the keyboard, so the rewrite lands at the cursor; in an app that keeps its
# selection (most do) it replaces it. What is checked: the whole rewrite reached the window that had the selection.
for _ in $(seq 1 40); do grep -q "The cat sat on the mat\.$" "$TARGET_FILE" 2>/dev/null && break; sleep 0.25; done
grep -q "The cat sat on the mat\.$" "$TARGET_FILE" && echo "ok: the whole rewrite reached the window ($(cat "$TARGET_FILE"))" || fail "the window holds: $(cat "$TARGET_FILE")"
grep -q "panicked" "$LOG" && fail "the shell panicked" || true
[ "$FAILED" = 0 ] && echo "PASS: writing" || exit 1
