#!/bin/bash
# Settings > Appearance > Accent colour: choosing a swatch is sent to the desktop service, Settings itself changes colour at once (the Sync login screen
# button is measured from a screenshot), and every swatch is offered.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-accent-test.sh tools/nested/run.sh settings-accent tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0; S=$SCRATCH/state
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.2; niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
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
button_colour() {  # the colour at the middle of the Sync login screen button, from a screenshot
  python3 - "$SCRATCH/$1.png" "$SCRATCH/state/layout.json" <<'PY'
import json, os, sys
from PIL import Image
d = json.load(open(sys.argv[2]))["elements"]["greeter-sync"]
ox, oy = (float(v) for v in os.environ["UI_ORIGIN"].split())
x, y = int(ox + d["x"] + 8), int(oy + d["y"] + d["h"] / 2)
print("%d %d %d" % Image.open(sys.argv[1]).convert("RGB").getpixel((x, y)))
PY
}
wait_id accent-blue 20 && echo "ok: the accent swatches are on the Appearance page" || fail "no swatches"
for name in blue purple pink red orange yellow green graphite; do has_id accent-$name || fail "missing swatch $name"; done
shot accent-blue; BLUE=$(button_colour accent-blue)
click_id accent-purple; sleep 2
grep -q 'UPDATE theme.accent "purple"' "$S/fake-settings-actions.log" && echo "ok: purple went to the desktop service" || fail "no update: $(cat "$S/fake-settings-actions.log")"
grep -q "UI theme .* accent purple" "$LOG" && echo "ok: and Settings heard it back" || fail "no accent in the program log"
shot accent-purple; PURPLE=$(button_colour accent-purple)
python3 - "$BLUE" "$PURPLE" <<'PY' && echo "ok: the button went from blue ($BLUE) to purple ($PURPLE)" || fail "colours: $BLUE -> $PURPLE"
import sys
b = [int(v) for v in sys.argv[1].split()]; p = [int(v) for v in sys.argv[2].split()]
assert b[2] > b[0] + 60, b            # blue: blue channel well above red
assert p[0] > 150 and p[2] > 180 and p[1] < 140, p   # purple: red and blue high, green low
PY
click_id accent-green; sleep 2
grep -q 'UPDATE theme.accent "green"' "$S/fake-settings-actions.log" && echo "ok: and a second choice replaces it" || fail "green not sent"
grep -q REFUSED "$S/fake-settings-actions.log" && fail "refused: $(grep REFUSED "$S/fake-settings-actions.log")" || echo "ok: nothing was refused"
[ "$FAILED" = 0 ] && echo "PASS: settings accent" || exit 1
