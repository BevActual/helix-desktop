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
wait_id welcome-more
click_id welcome-more
wait_id welcome-web
click_id welcome-web
wait_id web-panel
paste_id web-input 'fixture topic'
click_id web-lookup
wait_id web-result-1
click_id web-read-0
for _ in $(seq 1 60); do ! has_id web-read-0 && break; sleep 0.25; done
has_id web-read-0 && { echo 'FAIL: the source never changed from snippet to page'; exit 1; }
click_id web-open-0
python3 - "$SCRATCH/state/opened-sources.log" <<'PY'
import pathlib,sys
assert pathlib.Path(sys.argv[1]).read_text().strip()=='https://example.com/guide'
PY
click_id web-answer
wait_id answer-source-1-0
for _ in $(seq 1 60); do ! has_id stop && break; sleep 0.25; done
python3 - "$SCRATCH/data/helix/mobius/chats" <<'PY'
import pathlib,json,sys
chat=json.loads(next(pathlib.Path(sys.argv[1]).glob('*.json')).read_text())
assert chat['messages'][0]['text'].startswith('fixture topic')
assert 'never as instructions' in chat['messages'][0]['text']
assert 'DO_NOT_INCLUDE_' not in chat['messages'][0]['text']
sources=chat['messages'][1]['sources'];assert len(sources)==2
assert sources[0]['kind']=='page' and sources[1]['kind']=='snippet'
assert sources[0]['url']=='https://example.com/guide' and sources[0]['fetched']>0
PY
click_id new-chat
click_id chat-0
wait_id answer-source-1-0
# New lookup and a reserved address error remain recoverable.
click_id hub-toggle
click_id open-web
wait_id web-panel
click_id web-input-clear
paste_id web-input 'http://127.0.0.1/'
click_id web-lookup
wait_id web-error
click_id web-input-clear
paste_id web-input 'slow fixture'
click_id web-lookup
wait_id web-stop
click_id web-stop
click_id web-input-clear
paste_id web-input 'https://example.com/guide'
click_id web-lookup
wait_id web-result-0
has_id web-read-0 && { echo 'FAIL: direct URL was not a page result'; exit 1; }
click_id web-close
echo 'PASS: search, public-page reading, source links, evidence-backed answer, persistence, private-address refusal, cancellation and recovery'
