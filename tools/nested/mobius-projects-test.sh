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
fail() { echo "FAIL: $*"; exit 1; }
wait_id mode-projects
click_id mode-projects
wait_id projects-panel && wait_id project-new-name && echo "ok: the Projects sheet opened" || fail "no sheet"
click_id project-new-name; wtype "Homelab"; click_id project-new; sleep 1
wait_id project-instructions && echo "ok: the project was added and selected" || fail "no project detail"
click_id project-instructions; wtype "Prefer systemd units."; click_id project-save; sleep 1
click_id project-choose-folder; sleep 5
shot projects; sleep 1; shot projects
LAB=$SCRATCH/state/helix/mobius/lab.toml
python3 - "$LAB" "$SCRATCH/home/project" <<'PY' && echo "ok: instructions and folder are saved with the project" || fail "project not saved ($LAB)"
import tomllib, sys, os
lab = tomllib.load(open(sys.argv[1], 'rb'))
assert lab["projects"]["Homelab"] == "Prefer systemd units.", lab
assert os.path.realpath(lab["project_folders"]["Homelab"]) == os.path.realpath(sys.argv[2]), lab
PY
echo "--- a new chat in the project joins it and works in its folder"
click_id project-chat-new; sleep 1
click_id input; wtype "Hello from the project"; wtype -k Return; sleep 4
python3 - "$SCRATCH/data/helix/mobius/chats" "$SCRATCH/home/project" <<'PY' && echo "ok: the chat joined the project and got its folder" || fail "chat not in project"
import glob, json, sys, os
c = json.load(open(glob.glob(sys.argv[1] + "/*.json")[0]))
assert c["project"] == "Homelab", c["project"]
assert os.path.realpath(c["workspace"]) == os.path.realpath(sys.argv[2]), c["workspace"]
PY
click_id mode-projects; wait_id projects-panel; sleep 1
python3 -c "import json;e=json.load(open('$SCRATCH/state/layout.json'))['elements'];import sys;sys.exit(0 if any(k.startswith('project-chat-') and k!='project-chat-new' for k in e) else 1)" && echo "ok: the page lists the chat" || fail "chat not listed"
sleep 1; shot projects-final; sleep 1; shot projects-final
echo "PASS: mobius projects"
