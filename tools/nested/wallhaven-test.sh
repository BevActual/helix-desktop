#!/bin/bash
# Wallpaper page: search wallhaven.cc (real network), pick a picture, and check it is saved in the wallpaper folder and chosen.
#   HELIX_NESTED_SCRIPT=tools/nested/wallhaven-test.sh tools/nested/run.sh wallhaven tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
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
click_id page-wallpaper; sleep 1.5
has_id wh-query && echo "ok: the search box is there" || fail "no search box"
click_id wh-query; wtype "mountain lake"; wtype -k Return
HIT=""
for _ in $(seq 1 40); do
  HIT=$(python3 - "$LAYOUT" <<'PY'
import json, sys
e = json.load(open(sys.argv[1]))["elements"]
ids = sorted((v["y"], k) for k, v in e.items() if k.startswith("wh-hit-"))
print(ids[0][1] if ids else "")
PY
)
  [ -n "$HIT" ] && break; sleep 0.5
done
[ -n "$HIT" ] && echo "ok: results listed ($HIT)" || { fail "no results"; echo FAILED; exit 1; }
sleep 4; shot wallhaven-results
click_id "$HIT"
for _ in $(seq 1 60); do grep -q "SETTINGS wallpaper chosen" "$LOG" && break; sleep 0.5; done
grep -q "SETTINGS wallhaven saved" "$LOG" && echo "ok: the picture was downloaded" || fail "nothing was downloaded"
ID=${HIT#wh-hit-}
FILE=$(ls "$SCRATCH"/home/Pictures/Wallpapers/wallhaven-"$ID".* 2>/dev/null | head -1)
[ -n "$FILE" ] && echo "ok: saved as $(basename "$FILE")" || fail "no file in the wallpaper folder"
grep -q "wallhaven-$ID" "$SCRATCH/config/helix/wallpaper.toml" 2>/dev/null && echo "ok: it is the chosen wallpaper" || fail "the choice file does not name it"
[ "$(stat -c %a "$SCRATCH/config/helix/wallpaper.toml")" = 600 ] && echo "ok: choice file is private"
has_id "$HIT" && echo "ok: the results are still listed after choosing" || fail "results vanished"
shot wallhaven-chosen
click_id wh-surprise; sleep 6
has_id wh-more && echo "ok: a random search with no words lists pictures" || fail "surprise me found nothing"
shot wallhaven-surprise
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
