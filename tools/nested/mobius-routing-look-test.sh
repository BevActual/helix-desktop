#!/bin/bash
# Pictures of Mobius's model picker and routing page against the REAL service and models, with a copy of this machine's routing file (never written back).
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-routing-look-test.sh tools/nested/run.sh mobius-routing tools/nested/mobius-real-nested.sh <wide window>
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
source "$ROOT/tools/nested/ui-lib.sh"
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/route-$1.png"; }
geometry() {
  local loc; loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri", "msg", "--json", "windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri", "msg", "--json", "outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0, 0]; f = l.get("window_offset_in_tile") or [0, 0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
  export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
}
wait_id() { for _ in $(seq 1 ${2:-60}); do has_id "$1" && return 0; sleep 0.25; done; echo "missing $1"; return 1; }
wait_id composer; sleep 2; geometry
shot start
click_id meta-model; sleep 1.5; has_id menu-model-auto && echo 'menu: open' || echo 'menu: NOT open'; python3 -c "import json;d=json.load(open('$SCRATCH/state/layout.json'));print('menu at', {k:v for k,v in d['elements'].items() if k.startswith('menu-model') or k=='meta-model'}, d['width'], d['height'])"; shot picker; sleep 3; shot picker2; has_id menu-model-auto && echo 'menu: still open after the picture' || echo 'menu: CLOSED by the picture'
wtype -k Escape; sleep 1
click_id meta-more; click_id menu-more-route; sleep 2.5; shot routing
