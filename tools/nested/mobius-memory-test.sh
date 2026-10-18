#!/bin/bash
# Memory: /remember keeps a fact, /memory shows and edits the list, and the list is added to the system prompt of a model on this computer.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-memory-test.sh tools/nested/run.sh mobius-memory tools/nested/mobius-nested.sh <wide window> HELIX_MOBIUS_LOG_PROMPT=1
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
gone_id() { for _ in $(seq 1 24); do has_id "$1" || return 0; sleep 0.25; done; return 1; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
MEM=$SCRATCH/data/helix/mobius/memory.json
LOG=$SCRATCH/program.log
facts() { python3 -c "import json,sys;print('|'.join(i['text'] for i in json.load(open('$MEM'))['items']))" 2>/dev/null; }
echo "--- /remember keeps a fact"
click_id input; wtype "/remember I use Fedora with niri"; wtype -k Return; sleep 2
[ "$(facts)" = "I use Fedora with niri" ] && echo "ok: remembered" || fail "memory file: $(facts)"
echo "--- /memory shows it, and a second can be added there"
click_id input; wtype "/memory"; wtype -k Return
wait_id memory-panel && wait_id memory-input && echo "ok: the sheet opened" || fail "no memory sheet"
click_id memory-input; wtype "Answer in British English"; click_id memory-add; sleep 1
[ "$(facts)" = "I use Fedora with niri|Answer in British English" ] && echo "ok: added from the sheet" || fail "memory file: $(facts)"
click_id memory-done; sleep 1
echo "--- a model on this computer is told"
click_id input; wtype "Hello there"; wtype -k Return; sleep 3
grep -aq "MOBIUS memory-added" "$LOG" && echo "ok: the memory was added to the prompt" || fail "memory not sent"
echo "--- forgetting removes it"
click_id input; wtype "/memory"; wtype -k Return; wait_id memory-forget-1 && click_id memory-forget-1; sleep 1
[ "$(facts)" = "Answer in British English" ] && echo "ok: forgotten" || fail "memory file: $(facts)"
[ "$FAILED" = 0 ] && echo "PASS: mobius memory" || exit 1
