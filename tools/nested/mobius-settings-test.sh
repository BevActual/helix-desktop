#!/bin/bash
# Mobius Settings, Models: each job (Chat, Code, Pictures, ...) shows the model that does it; choosing one is saved at once (no Save button);
# a job with no suitable model installed offers one to get; the library narrows by what a model does.
#   tools/nested/ui-suite.sh mobius-settings
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
for _ in $(seq 1 80); do [ -f "$SCRATCH/state/layout.json" ] && has_id open-models && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|} UI_REVEAL="760 150"
# Two catalog models "installed" (sparse files of the right size) and one added by hand.
MODELS=$SCRATCH/data/helix/models; mkdir -p "$MODELS"
truncate -s 1117320736 "$MODELS/qwen2.5-1.5b-instruct-q4_k_m.gguf"
truncate -s 2104932768 "$MODELS/qwen2.5-3b-instruct-q4_k_m.gguf"
echo "--- 1. the jobs"
click_id open-models; sleep 1
wait_id job-chat && echo "ok: Settings opens on Models with a row per job" || { fail "no job rows"; shot settings-noopen; }
for job in chat code vision summarize act speech image; do has_id "job-$job" || fail "no row for $job"; done
has_id models-save && fail "there is still a Save button" || echo "ok: there is no Save button"
shot settings-models
echo "--- 2. a job with nothing suitable offers one to get"
has_id job-get-vision && echo "ok: Pictures offers a model that can see" || fail "no offer for Pictures"
has_id job-get-speech && echo "ok: Dictation offers the speech model" || fail "no offer for Dictation"
has_id job-get-chat && fail "Chat offers a download though models are installed" || echo "ok: Chat does not, a model is installed"
echo "--- 3. choosing a model is saved at once"
click_id job-pick-chat; sleep 0.8
wait_id job-opt-chat-qwen2.5-3b && echo "ok: the menu lists the installed models" || fail "no model in the menu"
shot settings-picker
click_id job-opt-chat-qwen2.5-3b; sleep 1.5
ROUTING=$(find "$SCRATCH/state" -name inference-routing.toml 2>/dev/null | head -1)
python3 - "$ROUTING" <<'PY' && echo "ok: the choice is in the routing file without pressing Save" || fail "chat model not saved"
import sys, tomllib
r = tomllib.load(open(sys.argv[1], "rb"))
p = r["profiles"]["chat"]
assert p["backend"] == "local" and p["model"] == "qwen2.5-3b-instruct-q4_k_m.gguf", p
assert r["profiles"]["act"]["backend"] == "local", r["profiles"]["act"]
PY
echo "--- 4. the library narrows"
click_id models-nav-library; sleep 0.8
click_id lib-filter-installed; sleep 1.2
shot settings-filter; shot settings-filter
has_id lib-card-qwen2.5-3b && ! has_id lib-card-llama-3.2-3b && echo "ok: Installed shows only installed models" || fail "the Installed filter did not narrow"
click_id lib-filter-speech; sleep 0.6
has_id lib-card-whisper-small && ! has_id lib-card-qwen2.5-3b && echo "ok: Speech shows the speech model" || fail "the Speech filter did not narrow"
click_id lib-filter-all; sleep 0.4
has_id lib-get-llama-3.2-3b && echo "ok: the library offers a model to get" || fail "no Get button in the library"
shot settings-library
echo "--- 5. the other sections"
click_id models-nav-generation; sleep 0.6
wait_id route-temperature && echo "ok: Generation has the reply settings" || fail "no generation settings"
click_id models-nav-providers; sleep 0.6
wait_id route-cloud-key && echo "ok: Providers has the network and cloud settings" || fail "no providers settings"
shot settings-providers
click_id models-back; sleep 1.2
has_id models-back && fail "Back did not close Settings" || echo "ok: Back closed Settings"
[ $FAILED = 0 ] && echo "PASS: mobius settings" || exit 1
