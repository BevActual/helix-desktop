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
wait_id meta-tools
click_id meta-tools
wait_id tools-panel
click_id tools-enabled
click_id tools-choose
click_id tools-save
paste_id input 'Research the guide then write a project note.'
click_id send
wait_id tool-approve
[ ! -e "$SCRATCH/home/project/proof.txt" ] || { echo 'FAIL: command ran without review'; exit 1; }
click_id tool-approve
for _ in $(seq 1 80); do ! has_id stop && break; sleep 0.25; done
python3 - "$SCRATCH" <<'PYTEST'
import json,pathlib,sys
p=pathlib.Path(sys.argv[1]);assert (p/'home/project/proof.txt').read_text()=='reviewed'
c=json.loads(next((p/'data/helix/mobius/chats').glob('*.json')).read_text())
assert c['tools_enabled'] and c['workspace'].endswith('/project')
a=c['messages'][-1]['activities'];assert [x['tool'] for x in a]==['web_search','web_read','terminal','answer']
assert all(x['state']=='completed' for x in a),a
assert c['messages'][-1]['sources'][0]['kind']=='page'
assert 'reviewed' in next(x['output'] for x in a if x['tool']=='terminal')
PYTEST
click_id new-chat
click_id chat-0
wait_id tool-terminal
# Declining the same proposed command leaves the project untouched.
printf '[{"tool":"terminal","command":"printf bad > declined.txt"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
paste_id input 'Make another note.'
click_id send
wait_id tool-approve
click_id tool-decline
for _ in $(seq 1 80); do ! has_id stop && break; sleep 0.25; done
[ ! -e "$SCRATCH/home/project/declined.txt" ] || { echo 'FAIL: declined command ran'; exit 1; }
# Stop during a real command ends the task and retains its audit.
printf '[{"tool":"terminal","command":"sleep 20; printf late > late.txt"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
paste_id input 'Run a long task.'
click_id send
wait_id tool-approve
click_id tool-approve
click_id stop
sleep 2
[ ! -e "$SCRATCH/home/project/late.txt" ] || { echo 'FAIL: stopped command completed'; exit 1; }
echo 'PASS: opt-in chat web tools, exact command review, real sandbox writes, sources, persistence, declined commands and cancellation'
