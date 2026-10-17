#!/bin/bash
# `@` in the composer: files of the chat's project folder and other chats, attached as chips.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-mention-test.sh tools/nested/run.sh mobius-mention tools/nested/mobius-tools-nested.sh <wide window>
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
CHATS=$SCRATCH/data/helix/mobius/chats
for _ in $(seq 1 80); do has_id input && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri", "msg", "--json", "windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri", "msg", "--json", "outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0, 0]; f = l.get("window_offset_in_tile") or [0, 0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
echo "--- 1. a first chat, then a project folder for the next one"
click_id input; wtype "Hello"; wtype -k Return
for _ in $(seq 1 80); do ls "$CHATS"/*.json >/dev/null 2>&1 && ! has_id stop && break; sleep 0.25; done
click_id mode; wait_id menu-mode-folder; click_id menu-mode-folder; wait_id tools-panel
click_id tools-enabled; click_id tools-choose; click_id tools-save; sleep 1
printf 'The plan is blue.\n' > "$SCRATCH/home/project/notes-plan.md"
printf 'x=1\n' > "$SCRATCH/home/project/main.py"
echo "--- 2. @ offers the project's files and attaches one as a chip"
click_id input; wtype "Summarise @notes"
wait_id slash-item-0 && echo "ok: the picker opened" || fail "no picker"
shot mention-menu; sleep 1; shot mention-menu
wtype -k Return; sleep 1
wait_id attach-chip-0 && echo "ok: the file is a chip" || fail "no chip"
has_id slash-item-0 && fail "the picker stayed open" || echo "ok: the picker closed"
shot mention-chip
echo "--- 3. @ offers other chats"
click_id new-chat; sleep 1; click_id input; wtype "@Hel"
wait_id slash-item-0 && echo "ok: a chat is offered" || fail "no chat offered"
wtype -k Return; sleep 1
wait_id attach-chip-0 && echo "ok: the chat is a chip" || fail "no chat chip"
[ "$FAILED" = 0 ] && echo "PASS: mobius mention" || exit 1
