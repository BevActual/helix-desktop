#!/bin/bash
set -eu
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
source "$ROOT/tools/nested/ui-lib.sh"
for _ in $(seq 1 80); do has_id chat-search && break; sleep 0.25; done
loc=$(python3 - <<'PY'
import json,subprocess
w=next(x for x in json.loads(subprocess.check_output(['niri','msg','--json','windows'])) if 'mobius' in (x['app_id'] or '').lower())
o=next(iter(json.loads(subprocess.check_output(['niri','msg','--json','outputs'])).values()))['logical']
l=w['layout'];t=l.get('tile_pos_in_workspace_view') or [0,0];f=l.get('window_offset_in_tile') or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
search() {
  click_id chat-search
  wl-copy --primary "$1"
  local pos; pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" chat-search)
  "$PT" $pos mclick
  sleep 1
}
search quartz
click_id chat-0
for _ in $(seq 1 40); do has_id find-hit && break; sleep 0.25; done
has_id find-hit || { echo 'FAIL: opening a search result did not highlight its match'; exit 1; }
click_id copy-1
wl-paste --no-newline > "$SCRATCH/search-reply.txt"
python3 - "$SCRATCH/search-reply.txt" <<'PY'
import pathlib,sys
assert pathlib.Path(sys.argv[1]).read_text()=='Answer 85: **quartz** appears here.', 'newest result must survive the 80-result limit'
PY
click_id find-close
# Native search-field clear control, avoiding WebKit keycode differences in nested tests.
click_id chat-search 0.95
sleep 0.5
search 'école'
has_id chat-0 || { echo 'FAIL: Unicode title search failed'; exit 1; }
has_id chat-1 && { echo 'FAIL: Unicode title search returned unrelated chats'; exit 1; }
click_id chat-search 0.95
sleep 0.5
search zzz-no-match
has_id chat-0 && { echo 'FAIL: stale results remained after changing the query'; exit 1; }
has_id chat-search-status || { echo 'FAIL: no empty-search feedback'; exit 1; }
echo 'PASS: search opens and highlights a message, keeps newest results, matches Unicode titles and clears stale results'
