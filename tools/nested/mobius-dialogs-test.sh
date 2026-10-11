#!/bin/bash
# Mobius sheets and persona rail (React): rename a chat, put it in a project, add a persona, open the model picker.
#   tools/nested/ui-suite.sh mobius-dialogs
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
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
has_id persona-dot-0 && has_id persona-dot-add && echo "ok: the persona rail is drawn" || fail "no persona rail"
click_id input; wtype "How does cron work?"; wtype -k Return
for _ in $(seq 1 120); do grep -q "MOBIUS turn ended" "$LOG" && break; sleep 0.25; done; sleep 1
echo "--- 1. rename"
click_id chat-title; sleep 0.8
shot rename-sheet
has_id rename-title && echo "ok: the rename sheet opened" || fail "no rename sheet"
wtype " renamed"; sleep 0.3; click_id rename-save; sleep 1.2
F=$(ls "$CHATS"/*.json 2>/dev/null | head -1)
grep -q "renamed" "$F" && echo "ok: the new title was saved" || fail "title not saved"
has_id rename-title && fail "the sheet stayed open" || echo "ok: the sheet closed"
echo "--- 2. project"
click_id project-chat; sleep 0.8
has_id project-name && echo "ok: the project sheet opened" || fail "no project sheet"
wtype "Homelab"; sleep 0.3; wtype -k Return; sleep 1.2
grep -q "Homelab" "$F" && echo "ok: the project was saved" || fail "project not saved"
for _ in $(seq 1 20); do has_id project-filter && break; sleep 0.25; done
has_id project-filter && echo "ok: the project filter appeared" || fail "no project filter"
echo "--- 3. a persona"
click_id persona-dot-add; sleep 0.8
has_id persona-edit-name && echo "ok: the persona sheet opened" || fail "no persona sheet"
shot persona-sheet
wtype " Two"; sleep 0.3; click_id persona-save; sleep 1.5
has_id persona-dot-1 && echo "ok: the new persona is on the rail" || fail "persona missing"
echo "--- 4. model picker"
click_id meta-model; sleep 1.2
has_id model-auto && echo "ok: the model picker opened" || fail "no model picker"
click_id model-auto; sleep 1
has_id model-auto && fail "the picker stayed open" || echo "ok: Auto closed the picker"
echo "--- 5. find in the chat"
click_id input; wtype "/find"; sleep 0.4; wtype -k Return; sleep 0.8
has_id find-input && echo "ok: the find bar opened" || fail "no find bar"
wtype "cron"; sleep 1
has_id find-hit && echo "ok: matches are highlighted in the thread" || fail "no highlighted match"
click_id find-close; sleep 0.6
has_id find-hit && fail "highlights stayed after closing find" || echo "ok: highlights cleared"
echo "--- 6. edit a message"
click_id edit-0; sleep 0.8
has_id edit-text && echo "ok: the edit sheet opened" || fail "no edit sheet"
wtype " please"; sleep 0.3; click_id edit-save; sleep 1.5
for _ in $(seq 1 60); do grep -c "MOBIUS turn ended" "$LOG" | grep -q "^[2-9]" && break; sleep 0.25; done
G=$(grep -l "please" "$CHATS"/*.json 2>/dev/null | head -1)
[ -n "$G" ] && echo "ok: the edited message was saved" || fail "edit not saved"
echo "--- 7. fork"
N=$(ls "$CHATS"/*.json | wc -l)
for _ in $(seq 1 40); do has_id fork-user-0 && break; sleep 0.25; done
click_id fork-user-0; sleep 1.5
M=$(ls "$CHATS"/*.json | wc -l)
[ "$M" -gt "$N" ] && echo "ok: a fork is a new chat ($N -> $M)" || fail "no fork ($N -> $M)"
echo "--- 8. slash menu"
click_id input; wtype "/"; sleep 0.8
has_id slash-item-0 && echo "ok: the slash menu lists commands" || fail "no slash menu"
wtype "mod"; sleep 0.6
has_id slash-item-0 && ! has_id slash-item-3 && echo "ok: typing narrows the commands" || fail "slash menu not narrowed"
wtype -k Escape; sleep 0.6
has_id slash-item-0 && fail "Escape left the slash menu open" || echo "ok: Escape closed the slash menu"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
