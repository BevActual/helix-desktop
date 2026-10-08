#!/bin/bash
# The Caps Lock palette against a real window: an emoji is typed into it, a clipboard entry is pasted into it, an action runs, and a
# sentence goes to the assistant.
#   HELIX_NESTED_SCRIPT=tools/nested/quick-insert-test.sh tools/nested/run.sh qi tools/nested/tauri-fake-desktop.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control; TARGET_FILE=$SCRATCH/target.txt; export TARGET_FILE
FAILED=0; TARGET=""
cleanup() { for p in $TARGET $OTHER $TERMW; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; true; }
trap cleanup EXIT
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1" >&2; return 1; }
target_is() { for _ in $(seq 1 40); do [ "$(cat "$TARGET_FILE" 2>/dev/null)" = "$1" ] && return 0; sleep 0.25; done; echo "target holds: [$(cat "$TARGET_FILE" 2>/dev/null)] wanted: [$1]" >&2; return 1; }
open_palette() { local before; before=$(grep -c "PANEL open" "$LOG"); echo quick-insert >> "$CONTROL"; wait_for "PANEL open" $((before + 1)) 40 || fail "the palette did not open"; sleep 0.8; }

wait_for "LIVE Sound" 1 120 || exit 1
python3 "$ROOT/tools/nested/text-target.py" & TARGET=$!
sleep 3   # the window maps and takes the keyboard

echo "--- 1. An emoji is typed into the window that had the keyboard"
open_palette
wtype "pizza"; sleep 0.8; wtype -k Return
wait_for "INSERT type 1 chars" 1 20 || fail "no emoji was chosen"
target_is "🍕" || fail "the emoji did not arrive"

echo "--- 2. A clipboard entry becomes the clipboard again and is pasted"
# Synthetic Ctrl+V / Shift+Insert do not reach GTK's paste binding in this nested harness (the keys arrive, the binding does not fire),
# so this checks what the palette is responsible for: the chosen entry is what is on the clipboard, and the paste was issued.
wl-copy "alpha beta"; sleep 1.2; wl-copy "second one"; sleep 1.2
open_palette
wtype -k Tab; sleep 0.8; wtype -k Down; sleep 0.4; wtype -k Return
wait_for "INSERT paste 10 chars" 1 20 || fail "no clipboard entry was chosen"
for _ in $(seq 1 20); do [ "$(timeout 3 wl-paste)" = "alpha beta" ] && break; sleep 0.25; done
[ "$(timeout 3 wl-paste)" = "alpha beta" ] || fail "the chosen entry is not on the clipboard: [$(timeout 3 wl-paste)]"

echo "--- 3. An action runs: do not disturb"
open_palette
wtype -k Tab; sleep 0.4; wtype -k Tab; sleep 0.5; wtype "focus"; sleep 0.8; wtype -k Return
wait_for "INSERT do ToggleDoNotDisturb" 1 20 || fail "the action was not chosen"
for _ in $(seq 1 40); do grep 'NOTIFICATIONS view' "$LOG" | tail -1 | grep -q 'dnd=true' && break; sleep 0.25; done
grep 'NOTIFICATIONS view' "$LOG" | tail -1 | grep -q 'dnd=true' || fail "do not disturb did not turn on"

echo "--- 4. A sentence goes to the assistant, and the panel it opens is the assistant's"
open_palette
wtype "make the screen dimmer"; sleep 0.8; wtype -k Return
wait_for "INSERT ask" 1 20 || fail "the sentence was not sent"
for _ in $(seq 1 20); do grep -q '"panel": *"assistant"' "$SCRATCH/state/layout.json" 2>/dev/null && break; sleep 0.25; done
grep -q '"panel": *"assistant"' "$SCRATCH/state/layout.json" || fail "the assistant panel did not open"

echo "--- 5. Text never goes to a window other than the one that had the keyboard when the palette opened"
echo close >> "$CONTROL"; sleep 1
OTHER_FILE=$SCRATCH/other.txt
open_palette
wtype "pizza"; sleep 0.8
APP_ID=other-window TARGET_FILE=$OTHER_FILE python3 "$ROOT/tools/nested/text-target.py" & OTHER=$!
sleep 3   # a second window appears while the palette is open and takes the keyboard
for _ in $(seq 1 20); do has_id qi-row-0 && break; sleep 0.25; done
click_id qi-row-0
wait_for "OSD notice Not inserted: the window changed" 1 20 || fail "no notice for the refused insertion"
sleep 1
[ -z "$(cat $OTHER_FILE 2>/dev/null)" ] && echo "ok: the other window received nothing" || fail "the other window received: [$(cat $OTHER_FILE)]"
kill $OTHER 2>/dev/null; sleep 1

echo "--- 6. Multi-line text is not pasted into a terminal"
TERM_FILE=$SCRATCH/term.txt
APP_ID=fake-terminal TARGET_FILE=$TERM_FILE python3 "$ROOT/tools/nested/text-target.py" & TERMW=$!
sleep 3
wl-copy $'first line\nsecond line'; sleep 1.2
open_palette
for _ in $(seq 1 20); do has_id qi-tab-clipboard && break; sleep 0.25; done
click_id qi-tab-clipboard; sleep 0.8
click_id qi-row-0
wait_for "OSD notice Multi-line text was not pasted into a terminal" 1 20 || fail "no notice for the refused paste"
[ -z "$(cat $TERM_FILE 2>/dev/null)" ] && echo "ok: nothing was pasted into the terminal" || fail "the terminal received: [$(cat $TERM_FILE)]"
kill $TERMW 2>/dev/null

echo "--- 7. What was typed is never logged"
grep -q 'make the screen dimmer\|alpha beta\|second one\|pizza' "$LOG" && fail "the log holds typed or copied text"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
