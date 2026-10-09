#!/bin/bash
# Settings > Dock: the page shows only what the shell honours, and a toggle / a typed size is written to the file the shell watches.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-dock-test.sh tools/nested/run.sh settings-dock tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
FILE=$SCRATCH/config/helix/dock-settings.json
for _ in $(seq 1 120); do grep -q "SETTINGS bluetooth" "$LOG" && break; sleep 0.25; done; sleep 2
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "settings" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
click_id page-dock; sleep 1.5; shot settings-dock
for id in auto_hide running indicators icon_size magnify magnification; do has_id "set-$id" && echo "ok: $id is on the page" || fail "no row for $id"; done
has_id set-dock.position && fail "a setting the shell ignores is still listed" || echo "ok: nothing the shell ignores"
click_id set-auto_hide; sleep 1
grep -q '"auto_hide":false' "$FILE" && echo "ok: Automatically hide written" || fail "auto_hide not written: $(cat "$FILE" 2>&1)"
click_id set-indicators; sleep 1
grep -q '"indicators":false' "$FILE" && grep -q '"auto_hide":false' "$FILE" && echo "ok: a second change keeps the first" || fail "second change lost the first: $(cat "$FILE")"
click_id set-icon_size-number; wtype "40"; wtype -k Return; sleep 1
grep -q '"icon_size":40' "$FILE" && echo "ok: a typed size written" || fail "size not written: $(cat "$FILE")"
click_id set-icon_size-number; wtype "500"; wtype -k Return; sleep 1
grep -q '"icon_size":40' "$FILE" && echo "ok: an out-of-range size is not kept" || fail "out of range accepted: $(cat "$FILE")"
[ "$(stat -c %a "$FILE")" = 600 ] && echo "ok: file is private"
shot settings-dock-after
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
