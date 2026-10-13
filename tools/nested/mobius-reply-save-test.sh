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

paste_id input 'Save this script for me.'
click_id send
for _ in $(seq 1 80); do ! has_id stop && break; sleep 0.25; done
wait_id save-reply-1
click_id save-reply-1
wait_id save-reply-panel
python3 - "$SCRATCH" <<'PY'
import pathlib,sys
draft=pathlib.Path(sys.argv[1])/'home'/'mobius-saved.py'
assert not draft.exists()
PY
click_id save-reply-confirm
python3 - "$SCRATCH" <<'PY'
import pathlib,sys
p=pathlib.Path(sys.argv[1])/'home'/'mobius-saved.py'
assert p.read_text().strip()=='print("saved")'
PY
wait_id save-code-block
click_id save-code-block
wait_id save-reply-panel
click_id save-reply-confirm
python3 - "$SCRATCH" <<'PY'
import pathlib,sys
p=pathlib.Path(sys.argv[1])/'home'/'mobius-snippet.py'
assert p.read_text().strip()=='print("saved")'
PY
echo 'PASS: reply save preview and per-block save write new files'
