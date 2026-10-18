#!/bin/bash
# A reasoning model (Qwen3 4B) loads in the bundled llama and its reasoning is folded into a "Thought for…" row, with the answer after it.
# Heavy; skipped when the model file is not there.   HELIX_THINK_MODEL=<file> tools/nested/ui-suite.sh mobius-think-real
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
LOG=$SCRATCH/program.log
REAL_HOME=$(getent passwd "$(id -u)" | cut -d: -f6)
MODEL=${HELIX_THINK_MODEL:-$REAL_HOME/.cache/helix-experiments/models/Qwen3-4B-Q4_K_M.gguf}
[ -f "$MODEL" ] || { echo "skip: no reasoning model at $MODEL"; exit 0; }
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
click_id input; wtype "What is 17 times 23? Work it out step by step."; wtype -k Return
START=$(date +%s)
for _ in $(seq 1 1200); do grep -q "MOBIUS turn ended" "$LOG" && break; sleep 0.5; done
echo "turn took $(( $(date +%s) - START )) s"
grep -a "MOBIUS turn ended" "$LOG" | tail -1
grep -q "MOBIUS turn ended Finished" "$LOG" && echo "ok: the model answered" || fail "the turn did not finish: $(tail -3 "$LOG" | cut -c1-200)"
sleep 1; shot think-real; shot think-real
F=$(ls -t "$SCRATCH"/data/helix/mobius/chats/*.json 2>/dev/null | head -1)
python3 - "$F" <<'PY' && echo "ok: the reply holds its reasoning and an answer containing 391" || fail "reply not as expected"
import json, sys
d = json.load(open(sys.argv[1])); reply = [m["text"] for m in d["messages"] if m["role"] == "assistant"][-1]
print("reply (start):", reply[:160].replace("\n", " "))
print("reply (end):", reply[-160:].replace("\n", " "))
assert "<think>" in reply and "</think>" in reply, "no reasoning block"
assert "391" in reply.split("</think>")[-1], "no answer after the reasoning"
PY
has_id thinking-toggle && echo "ok: the reasoning is a folded row" || echo "note: no folded-row id found (see the screenshot)"
[ $FAILED = 0 ] && echo "PASS: reasoning model" || exit 1
