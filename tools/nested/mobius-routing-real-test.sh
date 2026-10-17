#!/bin/bash
# Switching models in the real app against the REAL service and models: send with the routed model, switch from the model menu (one click), send again,
# and read from the saved replies which model wrote each. Heavy: loads two models. Skipped without two installed models.
#   MOBIUS_REAL_ROUTING=1 HELIX_NESTED_SCRIPT=tools/nested/mobius-routing-real-test.sh tools/nested/run.sh mobius-routing-real tools/nested/mobius-real-nested.sh <wide window> MOBIUS_REAL_ROUTING=1
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0; LOG=$SCRATCH/program.log
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 2; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/real-$1.png"; }
REAL_HOME=$(getent passwd "$(id -u)" | cut -d: -f6)
[ -f "$REAL_HOME/.local/share/helix/models/qwen2.5-1.5b-instruct-q4_k_m.gguf" ] || { echo "skip: the small model is not installed"; exit 0; }
CHATS=$SCRATCH/data/helix/mobius/chats
for _ in $(seq 1 80); do has_id input && break; sleep 0.25; done; sleep 2
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
turns() { grep -c "MOBIUS turn ended Finished" "$LOG"; }
wait_turn() { for _ in $(seq 1 $((${2:-240} * 2))); do [ "$(turns)" -ge "$1" ] && return 0; sleep 0.5; done; return 1; }
replies() { python3 - "$CHATS" <<'PY'
import glob, json, sys
f = glob.glob(sys.argv[1] + "/*.json")
if not f: print(""); raise SystemExit
for m in json.load(open(f[0]))["messages"]:
    if m["role"] == "assistant":
        s = m.get("stats") or {}
        print(f'{s.get("model","?")}|{s.get("tokens",0)}|{s.get("ms",0)}')
PY
}
echo "--- 1. the routed model answers (it has to load first)"
click_id input; wtype "Say hello in five words."; wtype -k Return
T0=$(date +%s); wait_turn 1 300 && echo "ok: answered after $(( $(date +%s) - T0 )) s" || fail "no answer from the routed model"
sleep 1; shot first
echo "--- 2. switch from the menu: one click"
click_id meta-model; sleep 1
wait_id menu-model-qwen2.5-1.5b-instruct-q4_k_m.gguf && echo "ok: the menu lists the installed models" || fail "menu has no 1.5B"
click_id menu-model-qwen2.5-1.5b-instruct-q4_k_m.gguf; sleep 1.5
has_id menu-model-auto && fail "the menu stayed open after choosing" || echo "ok: choosing closed the menu"
shot switched
echo "--- 3. the next message goes to the chosen model"
click_id input; wtype "Say goodbye in five words."; wtype -k Return
T1=$(date +%s); wait_turn 2 300 && echo "ok: answered after $(( $(date +%s) - T1 )) s" || fail "no answer after switching"
sleep 1; shot second
R=$(replies); echo "$R" | sed 's/^/reply: /'
[ "$(echo "$R" | sed -n 1p | cut -d'|' -f1)" != "$(echo "$R" | sed -n 2p | cut -d'|' -f1)" ] && echo "ok: two different models wrote the two replies" || fail "the same model answered both: $R"
echo "$R" | sed -n 2p | grep -q "^qwen2.5-1.5b-instruct-q4_k_m|" && echo "ok: the second reply is from the 1.5B" || fail "second reply model: $(echo "$R" | sed -n 2p)"
echo "--- 4. the choice belongs to this chat: a new chat uses the default again"
python3 - "$CHATS" <<'PY' && echo "ok: the first chat saved its own model" || fail "the first chat did not keep its model"
import glob, json, sys
chats = [json.load(open(f)) for f in glob.glob(sys.argv[1] + "/*.json")]
assert len(chats) == 1 and chats[0]["route_model"] == "qwen2.5-1.5b-instruct-q4_k_m.gguf", [c.get("route_model") for c in chats]
PY
click_id new-chat; sleep 1.5
click_id input; wtype "Say yes."; wtype -k Return
T2=$(date +%s); wait_turn 3 300 && echo "ok: answered after $(( $(date +%s) - T2 )) s" || fail "no answer in the new chat"
sleep 1
python3 - "$CHATS" <<'PY' && echo "ok: the new chat was answered by the default model, the first chat keeps the 1.5B" || fail "the new chat did not use the default"
import glob, json, sys
chats = [json.load(open(f)) for f in glob.glob(sys.argv[1] + "/*.json")]
assert len(chats) == 2, len(chats)
fresh = next(c for c in chats if c["messages"][0]["text"] == "Say yes.")
old = next(c for c in chats if c is not fresh)
assert fresh["route_model"] == "" and fresh["messages"][-1]["stats"]["model"].startswith("Qwen2.5-VL-7B"), (fresh["route_model"], fresh["messages"][-1]["stats"])
assert old["route_model"] == "qwen2.5-1.5b-instruct-q4_k_m.gguf"
PY
[ "$FAILED" = 0 ] && echo "PASS: mobius routing real" || exit 1
