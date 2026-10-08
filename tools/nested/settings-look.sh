#!/bin/bash
# Screenshots of every Settings page in the nested compositor (never the real screen).
#   HELIX_NESTED_SCRIPT=tools/nested/settings-look.sh tools/nested/run.sh settings-look tools/nested/settings-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log
source "$ROOT/tools/nested/ui-lib.sh"
for _ in $(seq 1 120); do grep -q "SETTINGS rows page=power" "$LOG" && break; sleep 0.25; done
sleep 2
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "settings" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
for page in appearance accessibility displays sound notifications power agents; do
  click_id page-$page; sleep 1.2
  niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.2
  ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/look-$page.png"
done
echo done
