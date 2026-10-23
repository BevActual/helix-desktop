#!/bin/bash
# Settings > Mouse & touchpad and Keyboard > Typing: a change is sent to the Helix compositor, takes effect there at once (asked back over IPC and, for the
# layout, in the compositor's own keyboard layouts), is remembered in input-overrides.json, and survives a reload of the configuration.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-pointer-test.sh tools/nested/run.sh settings-pointer tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
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
niri_input() { python3 - "$1" <<'PY'
import json, os, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["NIRI_SOCKET"]); s.send(sys.argv[1].encode() + b"\n"); s.settimeout(5)
print(json.dumps(json.loads(s.recv(1 << 20).decode())["Ok"]["HelixInput"]))
PY
}
field() { python3 -c 'import json,sys; print(json.loads(sys.argv[1])[sys.argv[2]])' "$(niri_input '"HelixInput"')" "$1"; }
for _ in $(seq 1 120); do grep -q "SETTINGS bluetooth" "$LOG" && break; sleep 0.25; done; sleep 2
OVR=$SCRATCH/state/helix/input-overrides.json
echo "--- Mouse & touchpad"
click_id page-pointer; sleep 1.5
wait_id set-touchpad:tap 20 && has_id set-touchpad:speed && has_id set-mouse:accel-flat && echo "ok: the touchpad and mouse rows are on the page" || fail "rows missing"
[ "$(field touchpad_tap)" = "False" ] && echo "ok: tap to click starts off, as the compositor reports" || fail "tap start: $(field touchpad_tap)"
shot pointer; sleep 1; shot pointer
click_id set-touchpad:tap; sleep 1.2
[ "$(field touchpad_tap)" = "True" ] && echo "ok: turning tap on took effect in the compositor" || fail "tap not on: $(niri_input '"HelixInput"')"
click_id set-mouse:accel-flat; sleep 1.2
[ "$(field mouse_accel_profile)" = "flat" ] && echo "ok: so did a flat mouse" || fail "accel: $(field mouse_accel_profile)"
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if d["touchpad_tap"] is True and d["mouse_accel_profile"]=="flat" else 1)' "$OVR" && echo "ok: both are remembered in input-overrides.json" || fail "overrides: $(cat "$OVR" 2>&1)"
[ ! -e "$SCRATCH/config/niri/helix-setup.kdl" ] || ! grep -q "touchpad" "$SCRATCH/config/niri/helix-setup.kdl" && echo "ok: and no KDL was written" || fail "KDL written"
echo "--- Keyboard: layout"
click_id page-keyboard; sleep 2
wait_id set-typing:layout 20 && echo "ok: the layout is on the Keyboard page" || fail "no layout row"
click_id set-typing:layout; wait_id set-typing:layout-filter 10 && wtype "german"; sleep 0.8
wait_id "set-typing:layout-de" 10 && click_id "set-typing:layout-de"; sleep 1.5
[ "$(field keyboard_layout)" = "de" ] && echo "ok: the German layout took effect" || fail "layout: $(field keyboard_layout)"
niri msg --json keyboard-layouts | grep -qi "german" && echo "ok: the compositor's own layouts say German" || fail "keyboard-layouts: $(niri msg --json keyboard-layouts)"
shot typing; sleep 1; shot typing
echo "--- it survives a reload of the configuration"
python3 - <<'PY'
import json, os, socket
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["NIRI_SOCKET"]); s.send(b'{"Action":{"LoadConfigFile":{}}}\n'); s.settimeout(5); s.recv(4096)
PY
sleep 2
[ "$(field keyboard_layout)" = "de" ] && [ "$(field touchpad_tap)" = "True" ] && echo "ok: after a reload the settings are still in effect" || fail "lost on reload: $(niri_input '"HelixInput"')"
[ "$FAILED" = 0 ] && echo "PASS: settings pointer" || exit 1
