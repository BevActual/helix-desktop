#!/bin/bash
# Code tasks: a request is listed as a task with its state (needs you while the agent waits, done after), and Follow opens the files the agent touches.
# A read outside the project is refused. Declining leaves the project alone.
#   tools/nested/ui-suite.sh mobius-acp
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-60}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
for _ in $(seq 1 80); do [ -f "$SCRATCH/state/layout.json" ] && has_id mode && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
NOTES=$SCRATCH/home/project/notes.txt
NOTES=$SCRATCH/home/project/notes.txt
click_id mode; wait_id menu-mode-folder && click_id menu-mode-folder; wait_id tools-panel
click_id tools-enabled; click_id tools-choose; click_id tools-save; sleep 1
click_id workspace; wait_id workspace-code && click_id workspace-code
wait_id code-agent-custom-fake-agent && click_id code-agent-custom-fake-agent; sleep 1
click_id code-follow; sleep 0.5
click_id code-tab-tasks; wait_id code-tasks && echo "ok: the Tasks tab is there" || fail "no tasks tab"
click_id input; wtype "Please add a line to the notes"; wtype -k Return
wait_id tool-approve 80 || fail "no permission card"
wait_id code-task-0-waiting 20 && echo "ok: the request is a task that needs the person" || { fail "task not waiting"; }
shot tasks-waiting; sleep 1.5; shot tasks-waiting
click_id tool-approve; sleep 2; wait_id tool-approve 60; click_id tool-approve; sleep 2; wait_id tool-approve 60; click_id tool-approve; sleep 3
for _ in $(seq 1 60); do has_id stop || break; sleep 0.5; done; sleep 1.5
wait_id code-task-0-done 20 && echo "ok: it is listed as done afterwards" || fail "task not done"
click_id code-tab-files; sleep 1
wait_id code-text 10 && echo "ok: Follow opened a file the agent touched" || fail "follow did not open a file"
shot tasks-done; sleep 1.5; shot tasks-done
[ $FAILED = 0 ] && echo "PASS: mobius tasks" || exit 1
