#!/bin/bash
# Mobius model library: the picker links to it, it lists models to download, and a vision model is among them.
#   tools/nested/ui-suite.sh mobius-library
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
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
has_id attach-picture && echo "ok: the composer has a picture button" || fail "no picture button"
click_id meta-model; sleep 1.2
has_id menu-model-routing && echo "ok: the model menu links to Models & routing" || fail "no routing link in the model menu"
click_id menu-model-routing; sleep 1.5
has_id models-nav-local && echo "ok: the library opened from the picker" || fail "the library did not open"
click_id models-nav-local; sleep 1.5
shot library
has_id lib-get-qwen2.5-3b && echo "ok: downloadable models are offered" || fail "no downloadable model card"
has_id lib-get-qwen2.5-vl-7b && echo "ok: a vision model is offered" || fail "no vision model card"
[ $FAILED = 0 ] && echo "PASS" || echo "FAILED"
