#!/bin/bash
# Settings > Displays > Arrangement: with two displays the page draws both; dragging one beside the other moves it in the compositor, snapped against its
# neighbour (never overlapping, never leaving a gap), and the new place is what the compositor reports and keeps.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-arrange-test.sh tools/nested/run.sh settings-arrange tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" HELIX_NESTED_EXTRA_OUTPUTS=1024x768
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "settings" in (x["app_id"] or "").lower())
o = json.loads(subprocess.check_output(["niri","msg","--json","outputs"]))["headless-1"]["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
every = [v["logical"] for v in json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()]
# The virtual pointer's 0..1 spans every display together.
W = max(l["x"] + l["width"] for l in every) - min(l["x"] for l in every); H = max(l["y"] + l["height"] for l in every) - min(l["y"] for l in every)
print(f'{t[0]+f[0]} {t[1]+f[1]}|{W} {H}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
pos() { niri msg --json outputs | python3 -c 'import json,sys; l=json.load(sys.stdin)[sys.argv[1]]["logical"]; print(l["x"], l["y"])' "$1"; }
for _ in $(seq 1 120); do grep -q "SETTINGS bluetooth" "$LOG" && break; sleep 0.25; done; sleep 2
click_id page-displays; sleep 2
wait_id arrange-headless-1 20 && has_id arrange-headless-2 && echo "ok: both displays are drawn in the arrangement" || fail "no arrangement: $(niri msg --json outputs | head -c 300)"
echo "start: headless-1 $(pos headless-1), headless-2 $(pos headless-2)"
shot arrange-before
# Drag headless-2 (wherever the compositor put it) down and to the left of headless-1: it should come to rest below, touching.
python3 - <<'PY' > "$SCRATCH/drag.txt"
import json, os
d = json.load(open(os.environ["XDG_STATE_HOME"] + "/layout.json"))["elements"]
a, b = d["arrange-headless-1"], d["arrange-headless-2"]
ox, oy = (float(v) for v in os.environ["UI_ORIGIN"].split())
sx, sy = ox + b["x"] + b["w"] / 2, oy + b["y"] + b["h"] / 2
ex, ey = ox + a["x"] + a["w"] / 2, oy + a["y"] + a["h"] + b["h"] / 2 + 6
# The virtual pointer's 0..1 spans every display together, not one of them.
import subprocess
ls = [o["logical"] for o in json.loads(subprocess.check_output(["niri", "msg", "--json", "outputs"])).values()]
w = max(l["x"] + l["width"] for l in ls) - min(l["x"] for l in ls)
h = max(l["y"] + l["height"] for l in ls) - min(l["y"] for l in ls)
print(f"{sx / w:.4f} {sy / h:.4f} {ex / w:.4f} {ey / h:.4f}")
PY
read SX SY EX EY < "$SCRATCH/drag.txt"
echo "drag $SX,$SY -> $EX,$EY"
"$PT" ${SX} ${SY} drag ${EX} ${EY}
sleep 2
sleep 1; read X2 Y2 <<< "$(pos headless-2)"; read X1 Y1 <<< "$(pos headless-1)"
sleep 1; echo "now: headless-1 $X1 $Y1, headless-2 $X2 $Y2"
grep -a "SETTINGS display" "$LOG" | tail -2
[ "$Y2" = "$((Y1 + 720))" ] && echo "ok: headless-2 now sits directly below headless-1, touching" || fail "not below: $X2 $Y2"
[ "$X2" -ge -1023 ] && [ "$X2" -le 1279 ] && echo "ok: and still faces it (x $X2)" || fail "x out of reach: $X2"
shot arrange-after
echo "--- the keyboard: the dragged screen has the focus, so the arrow keys move it"
# The drag left the keyboard focus on the screen that was dragged.
shot arrange-focus
wtype -k Right; sleep 2
read X2 Y2 <<< "$(pos headless-2)"; read X1 Y1 <<< "$(pos headless-1)"
echo "after Right: headless-1 $X1 $Y1, headless-2 $X2 $Y2"
[ "$X2" -ge "$((X1 + 1280))" ] && echo "ok: the right arrow put headless-2 to the right of headless-1" || fail "right arrow: $X2 $Y2"
wtype -k Down; sleep 2
read X2 Y2 <<< "$(pos headless-2)"
echo "after Down: headless-2 $X2 $Y2"
[ "$X2" -lt 1280 ] && [ "$Y2" -ge "$((Y1 + 720))" ] && echo "ok: the down arrow put it back below" || fail "down arrow: $X2 $Y2"
shot arrange-keys
[ "$FAILED" = 0 ] && echo "PASS: settings arrange" || exit 1
