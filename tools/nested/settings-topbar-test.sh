#!/bin/bash
# Settings > Top bar: every part of the bar has a switch; changing one writes bar-settings.json (which the shell watches), and the file keeps the rest.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-topbar-test.sh tools/nested/run.sh settings-topbar tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
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
FILE=$SCRATCH/config/helix/bar-settings.json
click_id page-topbar; sleep 1.5
wait_id set-bar:date 20 && has_id set-bar:apps && has_id set-bar:seconds && has_id set-bar:battery_percent && echo "ok: a switch for each part of the bar" || fail "rows missing"
shot topbar; sleep 1; shot topbar
[ ! -e "$FILE" ] && echo "ok: nothing is written until a choice is made" || fail "file exists: $(cat "$FILE")"
click_id set-bar:date; sleep 1.2
python3 -c 'import json,sys; d=json.load(open(sys.argv[1]))["show"]; sys.exit(0 if d=={"date":False} else 1)' "$FILE" && echo "ok: turning the date off wrote just that choice" || fail "file: $(cat "$FILE" 2>&1)"
click_id set-bar:seconds; sleep 1.2
python3 -c 'import json,sys; d=json.load(open(sys.argv[1]))["show"]; sys.exit(0 if d=={"date":False,"seconds":True} else 1)' "$FILE" && echo "ok: a second choice is added to it" || fail "file: $(cat "$FILE" 2>&1)"
[ "$FAILED" = 0 ] && echo "PASS: settings topbar" || exit 1
