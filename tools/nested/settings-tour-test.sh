#!/bin/bash
# A photograph of every Settings page, for review (not a pass/fail test): tools/nested/settings-tour.sh [dark|light]. Leaves tour-<page>.png in the scratch folder.
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log
source "$ROOT/tools/nested/ui-lib.sh"
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.2; niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/tour-$1.png"; }
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "settings" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
for _ in $(seq 1 120); do grep -q "SETTINGS bluetooth" "$LOG" && break; sleep 0.25; done; sleep 2
for page in appearance wallpaper accessibility displays network bluetooth sound keyboard pointer defaults startup datetime topbar dock notifications power agents about; do
  click_id "page-$page" 2>/dev/null; sleep 1.8; shot "$page"
done
echo "tour done"
