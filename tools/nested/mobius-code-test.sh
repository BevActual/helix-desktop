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
idle() { sleep 1; for _ in $(seq 1 80); do ! has_id stop && break; sleep 0.25; done; sleep 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
mkdir -p "$SCRATCH/home/project/src"
printf 'alpha\nbeta\n' > "$SCRATCH/home/project/notes.txt"
printf 'fn main() {\n    println!("hi");\n}\n' > "$SCRATCH/home/project/src/main.rs"
fail() { echo "FAIL: $*"; exit 1; }
wait_id mode
click_id mode; wait_id menu-mode-folder; click_id menu-mode-folder; wait_id tools-panel
click_id tools-enabled; click_id tools-choose; click_id tools-save; sleep 1
echo "--- the workspace menu opens the Code stage"
click_id workspace; wait_id workspace-code && echo "ok: the workspace menu lists Code" || fail "no workspace menu"
click_id workspace-code
wait_id code-stage && wait_id "code-file-notes.txt" && wait_id "code-file-src/main.rs" && echo "ok: the project's files are listed" || fail "no file list"
click_id "code-file-src/main.rs"; sleep 1
wait_id code-text && echo "ok: a file is shown" || fail "no viewer"
wait_id stage-chip && echo "ok: the file in view is attached as a chip" || fail "no chip"
shot code-view; sleep 1; shot code-view
echo "--- a change made by a turn is marked and shows its diff"
printf '[{"tool":"edit_file","path":"notes.txt","old":"beta","new":"gamma"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
paste_id input 'Change beta to gamma in notes.txt'
click_id send
wait_id tool-approve; click_id tool-approve; idle
wait_id "code-file-notes.txt" && click_id "code-file-notes.txt"; sleep 1
python3 - "$SCRATCH/state/layout.json" <<'PY' && echo "ok: the diff is shown with Revert" || fail "no diff view"
import json, sys
e = json.load(open(sys.argv[1]))["elements"]
assert "code-revert" in e, sorted(k for k in e if k.startswith("code"))
PY
shot code-diff; sleep 1; shot code-diff
click_id code-revert; sleep 2
[ "$(cat "$SCRATCH/home/project/notes.txt")" = "$(printf 'alpha\nbeta')" ] && echo "ok: reverted from the stage" || fail "not reverted: $(cat "$SCRATCH/home/project/notes.txt")"
echo "PASS: mobius code stage"
