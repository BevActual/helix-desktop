#!/bin/bash
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
capture() {
  # A size change requests a complete WebKit repaint in the nested software renderer.
  niri msg action set-window-width 800 >/dev/null
  sleep 0.6
  niri msg action set-window-width 850 >/dev/null
  sleep 0.6
  niri msg action screenshot-window --path "$SCRATCH/$1.png" >/dev/null
}
click_id attach
for _ in $(seq 1 40); do has_id attach-preview-0 && break; sleep 0.25; done
has_id attach-excerpt-note && has_id attach-warnings || { echo 'FAIL: excerpts or rejected selections are not visible'; exit 1; }
has_id attach-chip-3 && { echo 'FAIL: refused file was attached'; exit 1; }
capture cards
click_id attach-preview-1
has_id attachment-preview-panel && has_id attachment-preview-text || { echo 'FAIL: no file preview'; exit 1; }
capture preview
click_id attachment-preview-close
click_id send
for _ in $(seq 1 80); do has_id copy-1 && ! has_id stop && break; sleep 0.25; done
python3 - "$SCRATCH/data/helix/mobius/chats" <<'PY'
import json,pathlib,sys
chat=json.loads(next(pathlib.Path(sys.argv[1]).glob('*.json')).read_text())
text=chat['messages'][0]['text']
assert 'attached files are excerpts' in text
assert 'é'*6000 in text and 'é'*6001 not in text
assert 'b'*2000 in text and 'b'*2001 not in text
assert 'third.txt' not in text
assert 'SECRET_FIXTURE' not in text and 'binary.txt' not in text
PY
has_id attach-chip-0 && { echo 'FAIL: sent cards were not cleared'; exit 1; }
has_id attach-warnings && { echo 'FAIL: sent warnings were not cleared'; exit 1; }
click_id attach
for _ in $(seq 1 40); do has_id attach-remove-0 && break; sleep 0.25; done
click_id attach-remove-0
has_id attach-excerpt-note && { echo 'FAIL: removing the large file did not restore full remaining files'; exit 1; }
click_id attach-preview-0
has_id attachment-preview-panel || { echo 'FAIL: updated preview did not open'; exit 1; }
click_id attachment-preview-close
click_id attach-warnings-dismiss
has_id attach-warnings && { echo 'FAIL: warnings were not dismissed'; exit 1; }
capture compact
echo 'PASS: excerpt previews, partial selection errors, exact transmitted text, cleanup and space reclaimed after removal'
