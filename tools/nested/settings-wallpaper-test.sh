#!/bin/bash
# Settings > Wallpaper: All displays writes the default, and one display keeps its own colour without replacing that default.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-wallpaper-test.sh tools/nested/run.sh settings-wallpaper tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
place() {
  loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "settings" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
  export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
}
for _ in $(seq 1 120); do grep -q "SETTINGS bluetooth" "$LOG" && break; sleep 0.25; done; sleep 2
place
NAME=$(niri msg --json outputs | python3 -c 'import json,sys; print(next(iter(json.load(sys.stdin))))')
click_id page-wallpaper; sleep 1.5; shot settings-wallpaper
has_id wp-target-all && echo "ok: all displays is on the page" || fail "no all-displays control"
has_id "wp-target-${NAME}" && echo "ok: this display is on the page" || fail "no control for $NAME"
has_id wp-swatch-1 && echo "ok: a colour is on the page" || fail "no colour"
click_id wp-swatch-1; sleep 0.5
grep -q "SETTINGS wallpaper chosen" "$LOG" && echo "ok: all displays written" || fail "all-displays write missing"
CHOICE=$SCRATCH/config/helix/wallpaper.toml
grep -q 'color:#000000' "$CHOICE" && echo "ok: default colour kept" || fail "default missing: $(cat "$CHOICE" 2>&1)"
grep -q 'monitors' "$CHOICE" && fail "all displays left a per-display picture" || echo "ok: all displays has no per-display picture"
click_id "wp-target-${NAME}"; sleep 0.4
click_id wp-swatch-3; sleep 0.5
grep -q "SETTINGS wallpaper monitor ${NAME}" "$LOG" && echo "ok: this display written" || fail "monitor write missing: $(grep 'SETTINGS wallpaper' "$LOG" | tail -3)"
grep -q 'color:#000000' "$CHOICE" && grep -q 'color:#1f4d3a' "$CHOICE" && grep -q "$NAME" "$CHOICE" \
  && echo "ok: this display keeps its colour and the default stays" || fail "choice file: $(cat "$CHOICE" 2>&1)"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
