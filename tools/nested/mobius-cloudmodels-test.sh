#!/bin/bash
# Settings → Cloud models: OpenRouter's catalog (a saved copy, so nothing is fetched) can be searched and narrowed, a model is added to "my menu"
# or used for a job, one can be added by id, and the chat's model menu then lists what was added.
#   tools/nested/ui-suite.sh mobius-cloudmodels
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
MOBIUS=$SCRATCH/data/helix/mobius; mkdir -p "$MOBIUS"
python3 - "$MOBIUS/openrouter-models.json" <<'PY'
import json, sys, time
def m(id, name, created, **kw):
    base = dict(id=id, name=name, blurb=f"{name} is a test model.", context=128000, prompt_per_m=1.0, completion_per_m=2.0, free=False, vision=False,
                makes_images=False, tools=False, reasoning=False, created=created)
    base.update(kw); return base
models = [m("acme/vision-pro", "Acme Vision Pro", 300, vision=True, tools=True, context=1000000, prompt_per_m=2.5, completion_per_m=10.0),
          m("acme/thinker", "Acme Thinker", 200, reasoning=True),
          m("acme/small:free", "Acme Small (free)", 100, free=True, prompt_per_m=0.0, completion_per_m=0.0, context=32768),
          m("router/auto", "Auto Router", 50, prompt_per_m=None, completion_per_m=None)]
json.dump({"fetched": int(time.time()) - 3 * 86400, "models": models}, open(sys.argv[1], "w"))
PY
echo "--- 1. the catalog"
click_id open-models; sleep 1; wait_id models-nav-cloud && click_id models-nav-cloud; sleep 1
wait_id cloud-card-acme/vision-pro && echo "ok: the saved catalog is listed" || { fail "no catalog"; shot cloud-none; }
has_id cloud-card-router/auto && echo "ok: a model whose price varies is listed too" || fail "no router model"
shot cloudmodels; shot cloudmodels
echo "--- 2. narrowing"
click_id cloud-filter-free; sleep 0.6
has_id cloud-card-acme/small:free && ! has_id cloud-card-acme/vision-pro && echo "ok: Free shows only free models" || fail "the Free filter did not narrow"
click_id cloud-filter-vision; sleep 0.6
has_id cloud-card-acme/vision-pro && ! has_id cloud-card-acme/small:free && echo "ok: Sees pictures shows only those" || fail "the vision filter did not narrow"
click_id cloud-filter-all; sleep 0.4
echo "--- 3. my menu"
click_id cloud-pin-acme/vision-pro; sleep 1
python3 - "$MOBIUS/cloud-pins.json" <<'PY' && echo "ok: the model is in my menu (saved)" || fail "pin not saved"
import json, sys
assert json.load(open(sys.argv[1])) == ["acme/vision-pro"]
PY
echo "--- 4. use it for a job"
click_id job-pick-cloud-acme/vision-pro; sleep 0.8
wait_id cloud-use-acme/vision-pro-chat && click_id cloud-use-acme/vision-pro-chat; sleep 1.5
ROUTING=$(find "$SCRATCH/state" -name inference-routing.toml | head -1)
python3 - "$ROUTING" <<'PY' && echo "ok: Chat now uses the cloud model" || fail "chat route not saved"
import sys, tomllib
r = tomllib.load(open(sys.argv[1], "rb"))
assert r["profiles"]["chat"] == {"backend": "openrouter", "model": "acme/vision-pro"}, r["profiles"]["chat"]
assert r["profiles"]["act"]["backend"] == "local", r["profiles"]
PY
echo "--- 5. add by id"
click_id cloud-add-id; wtype "vendor/custom-model"; sleep 0.6
click_id cloud-add; sleep 1
python3 - "$MOBIUS/cloud-pins.json" <<'PY' && echo "ok: a model added by id is kept" || fail "custom id not saved"
import json, sys
assert json.load(open(sys.argv[1])) == ["acme/vision-pro", "vendor/custom-model"], open(sys.argv[1]).read()
PY
wait_id cloud-card-vendor/custom-model && echo "ok: it has a row, so it can be removed" || fail "no row for the custom model"
echo "--- 6. the chat's model menu"
click_id models-back; sleep 1.2
click_id meta-model; sleep 1.2
wait_id menu-cloud-acme/vision-pro && has_id menu-cloud-vendor/custom-model && echo "ok: the menu lists what was added" || fail "menu lacks the pinned models"
has_id menu-cloud-openai/gpt-4o-mini && fail "the menu still lists the default suggestions" || echo "ok: and not the default suggestions"
has_id menu-cloud-more && echo "ok: the menu leads to more" || fail "no more link"
shot cloudmenu; shot cloudmenu
[ $FAILED = 0 ] && echo "PASS: mobius cloud models" || exit 1
