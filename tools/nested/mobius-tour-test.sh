#!/bin/bash
# A tour of Mobius's main screens, photographed: home, a conversation, the model menu, the palette, every Settings section, the Code view with an
# agent's cards. Not a pass/fail test; it leaves tour-*.png in the scratch folder for review at whatever window width and theme it is run with.
#   tools/nested/tour.sh <width> <dark|light>
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.2; niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/tour-$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
for _ in $(seq 1 80); do [ -f "$SCRATCH/state/layout.json" ] && has_id input && break; sleep 0.25; done; sleep 1.5
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|} UI_REVEAL="760 150"
shot 01-home
click_id input; wtype "How do I schedule a job on Linux?"; wtype -k Return; sleep 6
shot 02-conversation
click_id meta-model 2>/dev/null; sleep 1.2; shot 03-model-menu
wtype -k Escape; sleep 0.5
click_id input; wtype -M ctrl -k k -m ctrl; sleep 1; shot 04-palette; wtype -k Escape; sleep 0.5
click_id open-models; sleep 1.5
n=05; for sec in models library cloud knowledge agents providers generation experiments; do
  wait_id "models-nav-$sec" 8 && click_id "models-nav-$sec"; sleep 1.2; shot "$n-settings-$sec"; n=$((n+1)); done
click_id models-back; sleep 1.2
click_id mode; wait_id menu-mode-folder && click_id menu-mode-folder; wait_id tools-panel 8 && { click_id tools-enabled; click_id tools-choose; click_id tools-save; sleep 1; }
click_id workspace; wait_id workspace-code 8 && click_id workspace-code; sleep 1.5; shot 13-code-stage
wait_id code-agent-custom-fake-agent 8 && click_id code-agent-custom-fake-agent; sleep 1
click_id input; wtype "Please add a line to the notes"; wtype -k Return; sleep 5; shot 14-agent-permission
echo "tour done"
