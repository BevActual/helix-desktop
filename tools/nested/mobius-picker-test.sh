#!/bin/bash
# Settings, Model picker: one list of every model with a switch each; turning one off removes it from the chat's model menu; searching reaches the OpenRouter
# catalog and a switch pins it; an id can be added.
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
json.dump({"fetched": int(time.time()) - 86400, "models": [m("acme/vision-pro", "Acme Vision Pro", 300, vision=True), m("acme/thinker", "Acme Thinker", 200, reasoning=True)]}, open(sys.argv[1], "w"))
PY
echo "--- 1. the list"
click_id open-models; sleep 1; wait_id models-nav-picker && click_id models-nav-picker; sleep 1.5
wait_id picker-list && has_id picker-switch-local:fake-model && echo "ok: the model on this computer is listed with a switch" || { fail "no local row"; shot picker-none; }
shot picker; sleep 1; shot picker
echo "--- 2. searching reaches OpenRouter, and a switch pins"
click_id picker-search; wtype "vision"; sleep 1
wait_id picker-switch-openrouter:acme/vision-pro 20 && echo "ok: searching lists a catalog model" || fail "catalog model not found"
click_id picker-switch-openrouter:acme/vision-pro; sleep 1.5
grep -q "acme/vision-pro" "$MOBIUS/cloud-pins.json" 2>/dev/null && echo "ok: its switch put it in the menu" || fail "not pinned"
echo "--- 3. adding by id"
click_id picker-search; wtype -M ctrl -k a -m ctrl; wtype "newco/model-x"; sleep 1
wait_id picker-add 20 && click_id picker-add; sleep 1.5
grep -q "newco/model-x" "$MOBIUS/cloud-pins.json" 2>/dev/null && echo "ok: a typed id was added" || fail "id not added"
echo "--- 4. turning a local model off removes it from the chat's menu"
click_id picker-search; wtype -M ctrl -k a -m ctrl; wtype -k BackSpace; sleep 1
click_id picker-switch-local:fake-model; sleep 1.5
grep -q "local:fake-model" "$MOBIUS/picker-hidden.json" 2>/dev/null && echo "ok: it is recorded as turned off" || fail "not recorded"
click_id models-back; sleep 1.5
click_id meta-model; wait_id menu-model 20 || fail "no model menu"
sleep 1; shot picker-menu; sleep 1; shot picker-menu
has_id menu-model-fake-model && fail "the turned-off model is still in the menu" || echo "ok: the menu no longer offers it"
has_id menu-cloud-setup && echo "ok: cloud models wait for a key (pins are kept)" || fail "no OpenRouter row"
[ "$FAILED" = 0 ] && echo "PASS: mobius picker" || exit 1
