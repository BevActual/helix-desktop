#!/bin/bash
# Screenshots of Mobius in the states a person meets, for looking at the interface (not an assertion test): the start screen, a conversation with a
# rich reply, a long one, the model picker, and a narrow window. Pictures are left in the run's scratch directory.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-look-test.sh tools/nested/run.sh mobius-look tools/nested/mobius-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
source "$ROOT/tools/nested/ui-lib.sh"
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/look-$1.png"; }
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
click_id composer; wtype "How does cron work, and when should I use a systemd timer instead?"; sleep 0.4
shot typed
click_id send
for _ in $(seq 1 80); do grep -q 'MOBIUS turn ended' "$SCRATCH/program.log" && break; sleep 0.25; done; sleep 1
shot reply
for i in 1 2 3; do click_id composer; wtype "Tell me more about point $i."; click_id send; sleep 3; done
shot long
niri msg action set-window-width 560 >/dev/null 2>&1; sleep 2; geometry
shot narrow
niri msg action set-window-width 1000 >/dev/null 2>&1; sleep 1.5; geometry
has_id new-chat && { click_id new-chat; sleep 1; shot start-second; }
echo "pictures in $SCRATCH/look-*.png"
