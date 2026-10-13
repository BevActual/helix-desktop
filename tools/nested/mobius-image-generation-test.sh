#!/bin/bash
# UI uses scripted diffusion with real PNGs and the installed model library; all state/clipboard is private.
set -u
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; exit 1; }
paste_id() {
  click_id "$1" || fail "no $1"
  wl-copy --primary "$2"
  local pos; pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1") || fail "no $1"
  "$PT" $pos mclick; sleep 0.5
}
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
mkdir -p "$SCRATCH/state/helix"
cat > "$SCRATCH/state/helix/inference-routing.toml" <<'TOML'
version = 1
[[backends]]
id = "unreachable-cloud"
kind = "openai"
base_url = "http://127.0.0.1:9/v1"
[profiles.chat]
backend = "unreachable-cloud"
model = "remote-model"
TOML
click_id hub-toggle; click_id open-create-image
has_id create-image-prompt || fail "Create image did not open"
paste_id create-image-prompt "a blue ceramic mug"
paste_id create-image-seed "42"
click_id create-image-generate || fail "no Generate image control"
for _ in $(seq 1 80); do has_id generated-image-again && break; sleep 0.25; done
has_id generated-image-again && has_id generated-image-copy && has_id generated-image-save || fail "no generated-image card/actions"
python3 - "$SCRATCH/data/helix/mobius" <<'PY' || fail "generated image was not persisted correctly"
import json,pathlib,struct,sys
base=pathlib.Path(sys.argv[1]);chat=json.loads(next((base/'chats').glob('*.json')).read_text())
assert chat['messages'][0]['text']=='/image a blue ceramic mug'
id=chat['messages'][1]['images'][0];png=(base/'images'/id).read_bytes()
assert png.startswith(b'\x89PNG\r\n\x1a\n') and struct.unpack('>II',png[16:24])==(512,512)
info=json.loads((base/'images'/f'{id}.json').read_text());assert info['seed']==42 and info['prompt']=='a blue ceramic mug'
assert (base/'images'/id).stat().st_mode & 0o777 == 0o600
print('ok: private PNG, seed and transcript persisted despite a remote chat route')
PY
click_id generated-image-copy
wl-paste --type image/png > "$SCRATCH/copied.png"
python3 - "$SCRATCH/copied.png" <<'PY' || fail "copy image did not place PNG bytes on the private clipboard"
import sys
assert open(sys.argv[1],'rb').read(8)==b'\x89PNG\r\n\x1a\n'
PY
echo "ok: Copy image uses PNG bytes"
click_id generated-image-prompt
has_id slash-item-0 || fail "Use as prompt did not put the image command in the composer"
# Reload the conversation from its sidebar row; the generated card must return.
click_id new-chat || fail "could not leave the image chat"
click_id chat-0 0.3 || fail "could not reopen the generated-image chat"
has_id generated-image || fail "saved generated image did not reopen"
click_id generated-image-again
click_id stop || fail "no Stop control while generating"
for _ in $(seq 1 80); do grep -q 'MOBIUS turn ended Stopped' "$SCRATCH/program.log" && break; sleep 0.25; done
grep -q 'MOBIUS turn ended Stopped' "$SCRATCH/program.log" || fail "generation cancellation did not finish"
echo "ok: Generate again and Stop work"
niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1
echo "PASS"
