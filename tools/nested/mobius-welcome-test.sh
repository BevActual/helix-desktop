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
click_id() {
  refresh_geometry
  local pos; pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" "${2:-0.5}")
  "$PT" $pos click; sleep 0.8
}
wait_id() { for _ in $(seq 1 60); do has_id "$1" && return; sleep 0.25; done; echo "FAIL: missing $1"; exit 1; }
wait_id welcome-files
for id in tools web picture image voice analysis brief models; do has_id "welcome-$id" || { echo "FAIL: no $id task"; exit 1; }; done
click_id welcome-tools
wait_id tools-panel
click_id tools-save
click_id suggestion-0
has_id msg-0 && { echo 'FAIL: starter sent without review'; exit 1; }
click_id suggestion-1
click_id send
for _ in $(seq 1 80); do grep -q 'MOBIUS turn ended Finished' "$SCRATCH/program.log" && break; sleep 0.25; done
python3 - "$SCRATCH/data/helix/mobius/chats" <<'PY'
import pathlib,json,sys
chats=[json.loads(p.read_text()) for p in pathlib.Path(sys.argv[1]).glob('*.json')]
assert len(chats)==1 and chats[0]['messages'][0]['text']=='Help me plan a focused week', 'a second starter must preserve the draft'
PY
click_id new-chat
wait_id welcome-image
click_id welcome-image
wait_id create-image-panel
click_id create-image-close
click_id welcome-analysis
wait_id analysis-panel
click_id analysis-close
click_id welcome-brief
wait_id context-panel
click_id context-close
click_id welcome-models
wait_id models-back
click_id models-back
click_id welcome-files
wait_id attach-chip-0
click_id attach-remove-0
click_id welcome-picture
wait_id picture-remove-0
click_id picture-remove-0
click_id welcome-voice
wait_id welcome-recording
click_id welcome-voice
for _ in $(seq 1 80); do grep -q 'MOBIUS dictated 19 characters' "$SCRATCH/program.log" && break; sleep 0.25; done
grep -q 'MOBIUS dictated 19 characters' "$SCRATCH/program.log" || { echo 'FAIL: welcome dictation did not transcribe'; exit 1; }
has_id msg-0 && { echo 'FAIL: dictation sent without review'; exit 1; }
click_id suggestion-0
click_id send
for _ in $(seq 1 80); do has_id msg-1 && ! has_id stop && break; sleep 0.25; done
python3 - "$SCRATCH/data/helix/mobius/chats" <<'PY'
import pathlib,json,sys
texts=[json.loads(p.read_text())['messages'][0]['text'] for p in pathlib.Path(sys.argv[1]).glob('*.json')]
assert 'heard 16000 samples' in texts, 'a starter must not overwrite dictated text'
PY
click_id new-chat
wait_id welcome-files
echo 'PASS: all welcome tool entry points, reviewed starters, protected drafts, local dictation and return to welcome'
