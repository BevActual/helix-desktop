#!/bin/bash
# With OpenRouter set up, the model menu lists cloud models (marked as leaving this computer) and one click puts this chat on one; the choice is
# saved with the chat and the routing file is left alone.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-cloud-test.sh tools/nested/run.sh mobius-cloud tools/nested/mobius-cloud-nested.sh <wide window>
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
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
ROUTING=$SCRATCH/state/helix/inference-routing.toml
BEFORE=$(sha256sum "$ROUTING" | cut -c1-64)
click_id input; wtype "Hello"; wtype -k Return; sleep 3
click_id meta-model; sleep 1
wait_id "menu-cloud-openai/gpt-4o-mini" && echo "ok: the menu lists OpenRouter models" || fail "no cloud models in the menu"
shot cloud-menu; sleep 1; shot cloud-menu
click_id "menu-cloud-google/gemini-2.5-flash"; sleep 2
python3 - "$CHATS" <<'PY' || fail "the route was not saved on the chat"
import glob, json, sys
c = json.load(open(glob.glob(sys.argv[1] + "/*.json")[0]))
assert (c["route_backend"], c["route_model"]) == ("openrouter", "google/gemini-2.5-flash"), c
print("ok: the chat is on the OpenRouter model")
PY
[ "$(sha256sum "$ROUTING" | cut -c1-64)" = "$BEFORE" ] && echo "ok: the routing file was not written" || fail "the routing file changed"
[ "$FAILED" = 0 ] && echo "PASS: mobius cloud" || exit 1
