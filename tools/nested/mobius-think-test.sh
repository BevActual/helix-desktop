#!/bin/bash
# A reasoning model's <think> text is folded into a "Thought it through" row above its answer.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-think-test.sh tools/nested/run.sh mobius-think tools/nested/mobius-nested.sh <wide window> "HELIX_FAKE_REPLY=<think>...</think>answer"
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
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
click_id input; wtype "What is 2 plus 2?"; wtype -k Return
wait_id thinking 60 && echo "ok: the thinking is a folded row" || fail "no thinking row"
for _ in $(seq 1 60); do ! has_id stop && break; sleep 0.5; done; sleep 2; shot think; sleep 1; shot think
click_id input; wtype "And 3 plus 3?"; wtype -k Return; sleep 4
grep -aq "MOBIUS system-prompt" "$SCRATCH/program.log" && true
python3 - "$SCRATCH/data/helix/mobius/chats" <<'PY' && echo "ok: the saved reply keeps the thinking" || fail "saved text lost the thinking"
import glob, json, sys
c = json.load(open(glob.glob(sys.argv[1] + "/*.json")[0]))
assert "<think>" in c["messages"][1]["text"], c["messages"][1]["text"]
PY
[ "$FAILED" = 0 ] && echo "PASS: mobius think" || exit 1
