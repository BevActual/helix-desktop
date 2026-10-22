#!/bin/bash
# The shell's own chrome follows the interface scale: the page is zoomed, what it reports back (sizes, positions) is in real pixels, and the sizes the
# compositor is told (the bar's exclusive zone) are scaled. Started with a scale of 1.5 from the scripted desktop service (HELIX_FAKE_UI_SCALE).
#   HELIX_NESTED_SCRIPT=tools/nested/shell-ui-scale-test.sh tools/nested/run.sh shell-ui-scale tools/nested/tauri-fake-desktop.sh HELIX_FAKE_UI_SCALE=1.5
SCRATCH=$1; LOG=$SCRATCH/program.log; LAYOUT=$SCRATCH/state/layout.json; FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
for _ in $(seq 1 120); do grep -q "READY shell" "$LOG" && break; sleep 0.25; done; sleep 4
SCALE=${HELIX_FAKE_UI_SCALE:-1.5}
grep -q "SHELL ui scale $SCALE" "$LOG" && echo "ok: the shell took the scale ($SCALE)" || fail "no scale in the log: $(grep -i 'scale' "$LOG" | head -3)"
python3 - "$LAYOUT" "$SCALE" <<'PY' && echo "ok: the page reports real pixels: the bar's clock is $(python3 -c 'import json,sys; print(round(json.load(open(sys.argv[1]))["elements"]["clock"]["h"]))' "$LAYOUT") px high at this scale" || fail "layout sizes"
import json, sys
d = json.load(open(sys.argv[1])); s = float(sys.argv[2])
assert abs(d.get("scale", 1) - s) < 0.01, d.get("scale")
clock = d["elements"]["clock"]
# At scale 1 the bar's clock is 26 px high.
assert abs(clock["h"] - 26 * s) < 3, clock
assert d["width"] == 1280, d["width"]
PY
RUN=$SCRATCH/state/run
[ "$(cat "$RUN/helix-ui-scale" 2>/dev/null)" = "$SCALE" ] && echo "ok: the scale is in the file the strut reads" || fail "scale file: $(cat "$RUN/helix-ui-scale" 2>&1)"
zone=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["exclusive_zone"])' "$RUN/helix-top-strut.json" 2>/dev/null)
want=$(python3 -c 'import sys; print(round(42 * float(sys.argv[1]) + 1e-9))' "$SCALE")
[ "$zone" = "$want" ] && echo "ok: the bar reserves $zone px (42 at scale 1)" || fail "bar zone $zone, wanted $want"
shot scaled
# The bar's lower part takes the pointer: at 1.5 the bar is 63 px tall, so a click 48 px down on the clock would fall through to nothing if the pointer
# region had stayed 42 px.
PT="$(cd "$(dirname "$0")/../.." && pwd)/tools/nested/wl-pointer/wl-pointer"
read -r CX CY <<< "$(python3 - "$LAYOUT" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); c = d["elements"]["clock"]
print(f'{(c["x"] + c["w"] / 2) / d["width"]:.4f} {(c["y"] + c["h"] - 4) / d["height"]:.4f}')
PY
)"
before=$(grep -c "PANEL open" "$LOG")
"$PT" "$CX" "$CY" click; sleep 1.5
[ "$(grep -c "PANEL open" "$LOG")" -gt "$before" ] && echo "ok: a click on the lower part of the scaled bar reached it" || fail "the click on the bar's lower part did nothing (pointer region too small?)"
[ "$FAILED" = 0 ] && echo "PASS: shell ui scale" || exit 1
