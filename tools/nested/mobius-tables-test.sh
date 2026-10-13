#!/bin/bash
# A scripted model answer exercises the real Rust parser, React table and private clipboard.
set -eu
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
source "$ROOT/tools/nested/ui-lib.sh"
for _ in $(seq 1 80); do has_id input && break; sleep 0.25; done
loc=$(python3 - <<'PY'
import json,subprocess
w=next(x for x in json.loads(subprocess.check_output(['niri','msg','--json','windows'])) if 'mobius' in (x['app_id'] or '').lower())
o=next(iter(json.loads(subprocess.check_output(['niri','msg','--json','outputs'])).values()))['logical']
l=w['layout'];t=l.get('tile_pos_in_workspace_view') or [0,0];f=l.get('window_offset_in_tile') or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
click_id input
wl-copy --primary 'Compare these models'
pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" input)
"$PT" $pos mclick
sleep 0.5
click_id send
for _ in $(seq 1 80); do has_id copy-table && ! has_id stop && break; sleep 0.25; done
has_id reply-table || { echo 'FAIL: no rendered table'; exit 1; }
click_id copy-table
wl-paste --no-newline > "$SCRATCH/table.tsv"
python3 - "$SCRATCH/table.tsv" <<'PY'
import pathlib,sys
assert pathlib.Path(sys.argv[1]).read_text() == 'Model\tCost\nSmall\t2|3\nLarge\t5', 'copy must preserve inline code pipes and remove Markdown emphasis'
PY
click_id new-chat
click_id chat-0
has_id reply-table || { echo 'FAIL: table missing after reopening'; exit 1; }
echo 'PASS: rendered comparison table, spreadsheet clipboard and saved chat reopening'
