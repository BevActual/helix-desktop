#!/bin/bash
# The Magic Pointer (docs/design/north-star.md, "intelligence in place"): point at a window and press the pointer key (`helix-shell "point"`);
# the compositor notes where the pointer was and the window under it, and the shell opens a bubble there. "What's this?" captures the window,
# cuts the patch around the pointer and asks the (scripted) vision model; an app's action that takes the selection runs for the person and
# is undone by the app's own call; Writing Tools open on the selection, already running. The nested compositor has no clipboard, so the
# selection is handed over in a file (HELIX_POINTER_TEST_SELECTION).   tools/nested/ui-suite.sh pointer
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
LOG=$SCRATCH/program.log; TARGET_FILE=$SCRATCH/target.txt; export TARGET_FILE
source "$ROOT/tools/nested/ui-lib.sh"; LAYOUT=$SCRATCH/state/layout.json
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1"; return 1; }
shot() { sleep 1; niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
notes_are() { for _ in $(seq 1 20); do python3 -c "import json,sys; sys.exit(json.load(open('$SCRATCH/notes.json')) != $1)" 2>/dev/null && return 0; sleep 0.25; done; return 1; }
wait_for "LIVE Sound" 1 120 || exit 1
mkdir -p "$XDG_DATA_HOME/helix/actions"
cat > "$XDG_DATA_HOME/helix/actions/notes.json" <<'JSON'
{ "provider": { "id": "notes", "title": "Notes", "bus": "org.example.Notes", "path": "/org/example/Notes", "apps": ["org.example.Notes"] },
  "actions": [
    { "id": "notes.add", "title": "Add to Notes", "summary": "Adds a note.", "risk": "act", "undo": "restore", "selection": "text",
      "params": { "type": "object", "properties": { "text": { "type": "string" } }, "required": ["text"] } },
    { "id": "notes.remove", "title": "Remove a note", "summary": "Removes a note.", "risk": "act", "undo": "none",
      "params": { "type": "object", "properties": { "text": { "type": "string" } }, "required": ["text"] } },
    { "id": "notes.list", "title": "List notes", "summary": "Lists the notes.", "risk": "observe", "undo": "none",
      "params": { "type": "object" } } ] }
JSON
echo '[]' > "$SCRATCH/notes.json"
python3 "$ROOT/tools/nested/notes-provider.py" "$SCRATCH/notes.json" & NOTES=$!
APP_ID=org.example.Notes python3 "$ROOT/tools/nested/text-target.py" & TARGET=$!
trap 'kill $TARGET $NOTES 2>/dev/null' EXIT
sleep 3
printf 'Milk' > "$SCRATCH/selection.txt"

echo "--- 1. the bubble opens where the pointer is, knowing the window under it"
"$PT" 0.3 0.5; sleep 0.5
niri msg action helix-shell point || fail "the compositor did not take the action"
wait_for "PANEL open" 1 20 || fail "the bubble did not open"
L=$(grep "POINTER at" "$LOG" | tail -1)
echo "$L" | grep -q "window org.example.Notes selection 4 chars, 2 offers" && echo "ok: ${L#*POINTER }" || fail "context: $L"
echo "$L" | grep -q "POINTER at nowhere" && fail "the compositor's note was not there"
sleep 1; shot pointer-open

echo "--- 2. What's this? looks at the patch around the pointer"
for _ in $(seq 1 40); do has_id pointer-describe && break; sleep 0.25; done
click_id pointer-describe
wait_for "POINTER picture" 1 40 && echo "ok: $(grep -o 'POINTER picture [0-9x]*' "$LOG" | tail -1)" || fail "no picture: $(grep POINTER "$LOG" | tail -3)"
wait_for "POINTER done" 1 60 && echo "ok: the answer came back" || fail "no answer: $(grep POINTER "$LOG" | tail -3)"
sleep 1; shot pointer-describe
click_id pointer-back   # the answer took the menu's place; back to it

echo "--- 3. an app's action on the selection, undone by the app"
click_id pointer-offer-notes.add
wait_for "POINTER ran notes.add" 1 20 && echo "ok: $(grep -o 'POINTER ran notes.add.*' "$LOG" | tail -1)" || fail "not run: $(grep POINTER "$LOG" | tail -2)"
notes_are "['Milk']" && echo "ok: Notes has it" || fail "notes: $(cat "$SCRATCH/notes.json")"
sleep 0.5; shot pointer-done
click_id pointer-undo
wait_for "POINTER undid" 1 20 || fail "undo not run"
notes_are "[]" && echo "ok: Undo took it back (the app's own call)" || fail "notes after undo: $(cat "$SCRATCH/notes.json")"
click_id pointer-back

echo "--- 4. Writing Tools on the selection, already running"
for _ in $(seq 1 20); do has_id pointer-writing-proofread && break; sleep 0.25; done
click_id pointer-writing-proofread
wait_for "WRITING selection 4 chars" 1 20 && echo "ok: Writing Tools opened on the selection" || fail "writing: $(grep WRITING "$LOG" | tail -2)"
wait_for "WRITING done" 1 60 && echo "ok: Proofread ran straight away" || fail "proofread: $(grep WRITING "$LOG" | tail -2)"
grep -q "panicked" "$LOG" && fail "the shell panicked" || true
[ "$FAILED" = 0 ] && echo "PASS: pointer" || exit 1
