#!/bin/bash
# The composer's harness modes (Agent, Ask, Plan, Desktop): the chip opens a menu, Shift+Tab cycles them, and Agent is the chat's tools setting.
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
echo "--- 2. the menu lists Agent, Ask, Plan and Desktop"
click_id mode; wait_id menu-mode-agent && wait_id menu-mode-ask && wait_id menu-mode-plan && wait_id menu-mode-act && wait_id menu-mode-folder && echo "ok: Agent, Ask, Plan, Desktop and the project folder" || fail "menu rows missing"
shot mode-menu; sleep 1; shot mode-menu
click_id menu-mode-agent; sleep 1
[ "$(tools_on)" = True ] && echo "ok: Agent saved tools on the chat" || fail "tools_enabled: $(tools_on)"
wait_id meta-tools && echo "ok: the folder chip appears while Agent is on" || fail "no folder chip"
echo "--- 3. Shift+Tab cycles Agent -> Ask -> Plan -> Desktop"
click_id input; shifttab
[ "$(tools_on)" = False ] && echo "ok: Ask turns tools off" || fail "tools_enabled after Ask: $(tools_on)"
has_id meta-tools && fail "the folder chip stayed" || echo "ok: the folder chip is gone"
shifttab
[ "$(tools_on)" = False ] && echo "ok: Plan does not turn tools on" || fail "tools_enabled after Plan: $(tools_on)"
shifttab
[ "$(tools_on)" = False ] && echo "ok: Desktop does not turn tools on" || fail "tools_enabled after Desktop: $(tools_on)"
echo "--- 4. Plan can be built in Agent"
click_id mode; wait_id menu-mode-plan && click_id menu-mode-plan; sleep 0.6
click_id input; wtype "Plan a small note"; wtype -k Return
for _ in $(seq 1 40); do has_id stop && break; sleep 0.25; done
for _ in $(seq 1 80); do has_id stop || break; sleep 0.25; done
click_id plan-build && echo "ok: Plan offers Build" || fail "no Build button"
sleep 1
[ "$(tools_on)" = True ] && echo "ok: Build switched to Agent" || fail "tools_enabled after Build: $(tools_on)"
[ "$FAILED" = 0 ] && echo "PASS: mobius mode" || exit 1
