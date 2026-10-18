#!/bin/bash
set -eu
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
source "$ROOT/tools/nested/ui-lib.sh"
refresh_geometry() {
  local loc; loc=$(python3 - <<'PY'
import json,subprocess
w=next(x for x in json.loads(subprocess.check_output(['niri','msg','--json','windows'])) if 'mobius' in (x['app_id'] or '').lower())
o=next(iter(json.loads(subprocess.check_output(['niri','msg','--json','outputs'])).values()))['logical']
l=w['layout'];t=l.get('tile_pos_in_workspace_view') or [0,0];f=l.get('window_offset_in_tile') or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
  export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
}
click_id() { refresh_geometry; local pos; pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" "${2:-0.5}"); "$PT" $pos click; sleep 0.8; }
wait_id() { for _ in $(seq 1 60); do has_id "$1" && return; sleep 0.25; done; echo "FAIL: missing $1"; exit 1; }
paste_id() { click_id "$1"; wl-copy --primary "$2"; refresh_geometry; local pos; pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1"); "$PT" $pos mclick; sleep 0.3; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
idle() { sleep 1; for _ in $(seq 1 80); do ! has_id stop && break; sleep 0.25; done; sleep 1; }
NOTES=$SCRATCH/home/project/notes.txt
printf 'alpha\nbeta\n' > "$NOTES"
printf 'Always answer in French.\n' > "$SCRATCH/home/project/AGENTS.md"
wait_id mode
click_id mode
wait_id menu-mode-folder
click_id menu-mode-folder
wait_id tools-panel
click_id tools-enabled
click_id tools-choose
wait_id tools-rules && echo "ok: the Tools sheet shows the project's rules" || { echo 'FAIL: no rules in the sheet'; exit 1; }
click_id tools-save
echo "--- an edit is shown as a diff and waits for review"
printf '[{"tool":"edit_file","path":"notes.txt","old":"beta","new":"gamma"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
paste_id input 'Change beta to gamma in notes.txt'
click_id send
wait_id tool-approve && echo "ok: waiting for review" || { echo "FAIL: no review"; exit 1; }
[ "$(cat "$NOTES")" = "$(printf 'alpha\nbeta')" ] && echo "ok: the file is untouched while waiting" || { echo "FAIL: changed before review"; exit 1; }
shot edit-review; sleep 1; shot edit-review
echo "--- applying it changes the file"
click_id tool-approve; idle
[ "$(cat "$NOTES")" = "$(printf 'alpha\ngamma')" ] && echo "ok: applied" || { echo "FAIL: not applied: $(cat "$NOTES")"; exit 1; }
wait_id tool-undo-edit && echo "ok: Undo is offered" || { echo "FAIL: no undo"; exit 1; }
shot edit-applied
echo "--- undo puts it back"
click_id tool-undo-edit; sleep 2
[ "$(cat "$NOTES")" = "$(printf 'alpha\nbeta')" ] && echo "ok: restored" || { echo "FAIL: not restored: $(cat "$NOTES")"; exit 1; }
echo "--- a declined edit changes nothing, and a path outside the folder fails"
printf '[{"tool":"edit_file","path":"notes.txt","old":"alpha","new":"no"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
paste_id input 'Try another edit'
click_id send
wait_id tool-approve; click_id tool-decline; idle
[ "$(cat "$NOTES")" = "$(printf 'alpha\nbeta')" ] && echo "ok: declined, unchanged" || { echo "FAIL: declined edit applied"; exit 1; }
printf '[{"tool":"edit_file","path":"../escape.txt","old":"","new":"x"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
paste_id input 'Write outside'
click_id send
wait_id tool-approve; click_id tool-approve; idle
[ ! -e "$SCRATCH/home/escape.txt" ] && echo "ok: nothing written outside the folder" || { echo "FAIL: escaped"; exit 1; }
echo "--- a command's changes are listed, with a way to revert each file"
printf '[{"tool":"terminal","command":"printf one > made.txt; printf ALPHA > notes.txt"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
paste_id input 'Run the command'
click_id send
wait_id tool-approve; click_id tool-approve; idle
wait_id changes-card && echo "ok: the Changes card appeared" || { echo "FAIL: no Changes card"; exit 1; }
click_id change-0; sleep 1; shot changes; sleep 1; shot changes
click_id change-revert-0; sleep 2
[ ! -e "$SCRATCH/home/project/made.txt" ] && echo "ok: the added file was reverted away" || { echo "FAIL: made.txt still there"; exit 1; }
[ "$(cat "$NOTES")" = "ALPHA" ] && echo "ok: the other file kept its change" || { echo "FAIL: notes: $(cat "$NOTES")"; exit 1; }
click_id change-revert-1; sleep 2
[ "$(cat "$NOTES")" = "$(printf 'alpha\nbeta')" ] && echo "ok: the modified file was restored" || { echo "FAIL: notes: $(cat "$NOTES")"; exit 1; }
echo "PASS: mobius edit"
