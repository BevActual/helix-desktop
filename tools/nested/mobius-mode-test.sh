#!/bin/bash
# The composer's mode (Ask, Tools, Act): the chip opens a menu, Shift+Tab cycles the three, and Tools is the chat's own setting (saved with the chat).
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-mode-test.sh tools/nested/run.sh mobius-mode tools/nested/mobius-nested.sh <wide window>
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
tools_on() { python3 - "$CHATS" <<'PY'
import glob, json, sys
files = glob.glob(sys.argv[1] + "/*.json")
print(json.load(open(files[0])).get("tools_enabled") if files else "none")
PY
}
shifttab() { wtype -M shift -k Tab -m shift; sleep 0.8; }
echo "--- 1. a chat starts in Ask, with the mode chip and no Act button"
has_id mode && echo "ok: the mode chip is there" || fail "no mode chip"
has_id act && fail "the old Act button is still there" || echo "ok: no separate Act button"
click_id input; wtype "Hello"; wtype -k Return
for _ in $(seq 1 80); do [ "$(tools_on)" != none ] && ! has_id stop && break; sleep 0.25; done
[ "$(tools_on)" = False ] && echo "ok: Tools are off" || fail "tools_enabled: $(tools_on)"
echo "--- 2. the menu lists the modes, and Tools switches on from it"
click_id mode; wait_id menu-mode-tools && wait_id menu-mode-act && wait_id menu-mode-folder && echo "ok: Ask, Tools, Act and the project folder" || fail "menu rows missing"
shot mode-menu; sleep 1; shot mode-menu
click_id menu-mode-tools; sleep 1
[ "$(tools_on)" = True ] && echo "ok: Tools saved on the chat" || fail "tools_enabled: $(tools_on)"
wait_id meta-tools && echo "ok: the folder chip appears while Tools are on" || fail "no folder chip"
echo "--- 3. Shift+Tab cycles Tools -> Act -> Ask"
click_id input; shifttab
[ "$(tools_on)" = True ] && echo "ok: Act leaves the chat's Tools setting alone" || fail "tools_enabled after Act: $(tools_on)"
shifttab
[ "$(tools_on)" = False ] && echo "ok: back in Ask, Tools are off" || fail "tools_enabled after Ask: $(tools_on)"
has_id meta-tools && fail "the folder chip stayed" || echo "ok: the folder chip is gone"
[ "$FAILED" = 0 ] && echo "PASS: mobius mode" || exit 1
