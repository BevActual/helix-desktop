#!/bin/bash
# A coding agent (a pretend one) works in the project through Mobius: what it says streams into the thread, its thoughts fold, and everything it asks of
# the computer waits for the person: its permission question, the file it wants to write (a diff to apply, with Undo), and the command it wants to run.
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
echo "--- 1. Settings lists the agent"
click_id open-models; sleep 1; wait_id models-nav-agents && click_id models-nav-agents; sleep 1
wait_id agent-custom-fake-agent && has_id agent-trust-custom-fake-agent && echo "ok: the agent is listed with its trust switch" || fail "agent not listed"
click_id agent-test-custom-fake-agent; sleep 3
wait_id agent-test-ok-custom-fake-agent && echo "ok: Test starts it and shows what it says it is" || fail "no test result"
shot agents; shot agents
click_id models-back; sleep 1.2
echo "--- 2. choose a project, then the agent"
click_id mode; wait_id menu-mode-folder && click_id menu-mode-folder; wait_id tools-panel
click_id tools-enabled; click_id tools-choose; click_id tools-save; sleep 1
click_id workspace; wait_id workspace-code && click_id workspace-code
wait_id code-stage && wait_id code-agent-custom-fake-agent && echo "ok: the Code view offers the agent" || fail "no agent chooser"
click_id code-agent-custom-fake-agent; sleep 1
echo "--- 3. a turn: it asks for everything and waits"
click_id input; wtype "Please add a line to the notes"; wtype -k Return
wait_id tool-approve 80 && echo "ok: its permission question is put to the person" || { fail "no permission card"; shot acp-none; }
shot acp-permission; shot acp-permission
[ "$(cat "$NOTES")" = "$(printf 'alpha\nbeta')" ] && echo "ok: nothing has been written yet" || fail "notes changed early"
click_id tool-approve; sleep 2
wait_id tool-approve 60 && echo "ok: then the file it wants to write, as a change to review" || fail "no edit card"
shot acp-edit; shot acp-edit
[ "$(cat "$NOTES")" = "$(printf 'alpha\nbeta')" ] && echo "ok: still unwritten while it waits" || fail "notes changed before approval"
click_id tool-approve; sleep 2
wait_id tool-approve 60 && echo "ok: then the command it wants to run" || fail "no command card"
click_id tool-approve; sleep 3
for _ in $(seq 1 60); do has_id stop || break; sleep 0.5; done; sleep 1
[ "$(cat "$NOTES")" = "$(printf 'alpha\nbeta\nedited by the agent')" ] && echo "ok: the approved edit was written" || fail "notes not as expected: $(cat "$NOTES")"
F=$(ls -t "$SCRATCH"/data/helix/mobius/chats/*.json | head -1)
python3 - "$F" <<'PY' && echo "ok: the reply, its folded thoughts and its cards are in the thread" || fail "thread not as expected"
import json, sys
m = json.load(open(sys.argv[1]))["messages"][-1]
assert m["text"].startswith("<think>Let me look at the notes first.</think>"), m["text"]
assert "read:alpha|beta" in m["text"] and "outside:refused" in m["text"] and "wrote:ok" in m["text"] and "ls:ok" in m["text"], m["text"]
tools = {a["tool"]: a["state"] for a in m["activities"]}
assert tools.get("edit_file") == "completed" and tools.get("terminal") == "completed" and tools.get("agent_permission") == "completed", tools
assert any(a["tool"] == "read" and a["state"] == "completed" for a in m["activities"]), m["activities"]
assert any(a["tool"] == "planning" for a in m["activities"]), m["activities"]
PY
shot acp-done; shot acp-done
echo "--- 4. Undo restores the file"
wait_id tool-undo-edit && click_id tool-undo-edit; sleep 1.5
[ "$(cat "$NOTES")" = "$(printf 'alpha\nbeta')" ] && echo "ok: Undo put the file back" || fail "undo failed: $(cat "$NOTES")"
echo "--- 5. declining leaves the project alone"
click_id input; wtype "Try again"; wtype -k Return
wait_id tool-decline 80 && click_id tool-decline; sleep 2
wait_id tool-decline 60 && click_id tool-decline; sleep 3
for _ in $(seq 1 60); do has_id stop || break; sleep 0.5; done; sleep 1
[ "$(cat "$NOTES")" = "$(printf 'alpha\nbeta')" ] && echo "ok: declined, so the file is unchanged" || fail "notes changed after declining: $(cat "$NOTES")"
python3 - "$F" <<'PY' && echo "ok: the agent was told it was not allowed" || fail "decline not reflected"
import json, sys
m = json.load(open(sys.argv[1]))["messages"][-1]
assert "wrote:not-allowed" in m["text"] and "ls:declined" in m["text"], m["text"]
PY
[ $FAILED = 0 ] && echo "PASS: mobius acp" || exit 1
