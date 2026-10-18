#!/bin/bash
# Mobius pictures: a saved chat shows its picture, and editing the message sends the picture again through the real service
# (the scripted backend answers with the size it received).
#   tools/nested/ui-suite.sh mobius-pictures
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
LOG=$SCRATCH/program.log
CHATS=$SCRATCH/data/helix/mobius/chats
for _ in $(seq 1 80); do [ -f "$SCRATCH/state/layout.json" ] && has_id input && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
for _ in $(seq 1 40); do has_id chat-0 && break; sleep 0.25; done
click_id chat-0; sleep 1.5
for _ in $(seq 1 40); do has_id msg-picture && break; sleep 0.25; done
has_id msg-picture && echo "ok: the saved picture is drawn in the chat" || fail "no picture in the message"
click_id edit-0; sleep 0.8
has_id edit-text && echo "ok: the message became an editable box in place" || fail "no inline edit box"
click_id edit-save; sleep 1.5
for _ in $(seq 1 80); do grep -q "I can see 1 image" "$CHATS"/00000000000000c1.json 2>/dev/null && break; sleep 0.25; done
grep -q "I can see 1 image(s): 40x30" "$CHATS"/00000000000000c1.json && echo "ok: the service received the picture, decoded (40x30)" || fail "the picture did not reach the model"
[ $FAILED = 0 ] && echo "PASS" || echo "FAILED"
