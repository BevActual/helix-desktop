#!/bin/bash
set -eu
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
source "$ROOT/tools/nested/ui-lib.sh"
for _ in $(seq 1 80); do has_id input && break; sleep 0.25; done
refresh_geometry() {
loc=$(python3 - <<'PY'
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
  local pos
  pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" "${2:-0.5}") || { echo "NOT FOUND: $1"; return 1; }
  "$PT" $pos click
  sleep 0.8
}
paste_id() {
  click_id "$1"
  wl-copy --primary "$2"
  refresh_geometry
  local pos; pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1")
  "$PT" $pos mclick
  sleep 0.3
}
click_id chat-0
has_id context-boundary || { echo 'FAIL: no recent-history boundary'; exit 1; }
click_id meta-more; click_id menu-more-context
for _ in $(seq 1 40); do has_id context-panel && break; sleep 0.25; done
has_id context-history-notice || { echo 'FAIL: omitted history is not explained'; exit 1; }
paste_id context-brief 'Goal: keep Helix offline. Decision: retain GPU sandboxing.'
click_id context-save
python3 - "$SCRATCH/data/helix/mobius/chats/00000000000000c1.json" <<'PY'
import json,pathlib,sys
p=pathlib.Path(sys.argv[1]);chat=json.loads(p.read_text())
assert chat['brief']=='Goal: keep Helix offline. Decision: retain GPU sandboxing.'
assert p.stat().st_mode & 0o777 == 0o600
PY
paste_id input 'What should we keep in mind?'
click_id send
click_id meta-more; click_id menu-more-context
has_id context-streaming-notice || { echo 'FAIL: brief editing is not locked during a reply'; exit 1; }
click_id context-close
click_id stop
for _ in $(seq 1 80); do ! has_id stop && break; sleep 0.25; done
grep -q 'MOBIUS system-prompt .*Goal: keep Helix offline. Decision: retain GPU sandboxing.' "$SCRATCH/program.log" || { echo 'FAIL: brief was not sent to inference'; exit 1; }
click_id new-chat
click_id chat-0
click_id meta-more; click_id menu-more-context
for _ in $(seq 1 40); do has_id context-panel && break; sleep 0.25; done
has_id context-history-notice || { echo 'FAIL: saved history report missing'; exit 1; }
click_id context-brief-clear
click_id context-save
python3 - "$SCRATCH/data/helix/mobius/chats/00000000000000c1.json" <<'PY'
import json,sys
assert json.load(open(sys.argv[1]))['brief']==''
PY
# A brief-only draft is saved and remains accessible before its first message.
click_id new-chat
click_id meta-more; click_id menu-more-context
paste_id context-brief 'Draft goal: plan tomorrow.'
click_id context-save
click_id new-chat
click_id chat-0
click_id meta-more; click_id menu-more-context
has_id context-brief-clear || { echo 'FAIL: brief-only draft could not be reopened'; exit 1; }
click_id context-brief-clear
click_id context-save
python3 - "$SCRATCH/data/helix/mobius/chats" <<'PY'
import pathlib,sys
assert not any('Draft goal: plan tomorrow.' in p.read_text() for p in pathlib.Path(sys.argv[1]).glob('*.json'))
PY
# Temporary briefs never create a saved conversation.
click_id temp-chat
click_id meta-more; click_id menu-more-context
paste_id context-brief 'Temporary private goal.'
click_id context-save
python3 - "$SCRATCH/data/helix/mobius/chats" <<'PY'
import pathlib,sys
assert not any('Temporary private goal.' in p.read_text() for p in pathlib.Path(sys.argv[1]).glob('*.json'))
PY
echo 'PASS: recent-history report, retained brief, inference prompt, streaming lock, clear/reopen, brief-only draft and temporary privacy'
