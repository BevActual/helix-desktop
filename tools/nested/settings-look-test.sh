#!/bin/bash
# Settings > Appearance > Windows and cursor: a change is sent to the Helix compositor, takes effect there at once (asked back over IPC), is remembered in
# look-overrides.json and survives a reload of the configuration; choosing an accent colour recolours the focus ring.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-look-test.sh tools/nested/run.sh settings-look tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
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
ipc() { python3 - "$1" <<'PY'
import json, os, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["NIRI_SOCKET"]); s.send(sys.argv[1].encode() + b"\n"); s.settimeout(5)
r = json.loads(s.recv(1 << 20).decode())
ok = r.get("Ok")
print(json.dumps(ok.get("HelixLook", ok) if isinstance(ok, dict) else (ok if ok is not None else r)))
PY
}
field() { python3 -c 'import json,sys; print(json.loads(sys.argv[1])[sys.argv[2]])' "$(ipc '"HelixLook"')" "$1"; }
for _ in $(seq 1 120); do grep -q "SETTINGS bluetooth" "$LOG" && break; sleep 0.25; done; sleep 2
OVR=$SCRATCH/state/helix/look-overrides.json
wait_id set-look:animations 20 && has_id set-look:gaps && has_id set-look:cursor && echo "ok: the animation, gap and cursor rows are on Appearance" || fail "rows missing"
[ "$(field animations)" = "True" ] && [ "$(field window_gaps)" = "0" ] && echo "ok: they start as the compositor reports (animations on, gaps 0 from config.kdl)" || fail "start: $(ipc '"HelixLook"')"
shot look; sleep 1; shot look
click_id set-look:animations; sleep 1.2
[ "$(field animations)" = "False" ] && echo "ok: turning animations off took effect in the compositor" || fail "animations: $(ipc '"HelixLook"')"
# (the gaps move the window, so the accent is chosen first)
click_id accent-purple; sleep 2
[ "$(field focus_color)" = "#bf5af2" ] && echo "ok: choosing purple recoloured the focus ring" || fail "ring: $(field focus_color)"
ipc '{"HelixSetLook":{"window_gaps":12,"cursor_size":32}}' >/dev/null; sleep 0.5
[ "$(field window_gaps)" = "12" ] && [ "$(field cursor_size)" = "32" ] && echo "ok: gaps and cursor size apply at once" || fail "gaps/cursor: $(ipc '"HelixLook"')"
r=$(ipc '{"HelixSetLook":{"window_gaps":900}}'); echo "$r" | grep -qi "gaps are 0 to 64" && echo "ok: an impossible gap is refused" || fail "no refusal: $r"
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if d["animations"] is False and d["window_gaps"]==12 and d["focus_color"]=="#bf5af2" else 1)' "$OVR" && echo "ok: all of it is remembered in look-overrides.json" || fail "overrides: $(cat "$OVR" 2>&1)"
echo "--- it survives a reload of the configuration"
python3 - <<'PY'
import json, os, socket
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["NIRI_SOCKET"]); s.send(b'{"Action":{"LoadConfigFile":{}}}\n'); s.settimeout(5); s.recv(4096)
PY
sleep 2
[ "$(field animations)" = "False" ] && [ "$(field window_gaps)" = "12" ] && [ "$(field focus_color)" = "#bf5af2" ] && echo "ok: after a reload the look is still in effect" || fail "lost on reload: $(ipc '"HelixLook"')"
echo "--- glass: a slider from Clear to Tinted, written for the shell"
SHAPE=$SCRATCH/config/helix/shape-settings.json
wait_id set-look:glass-number 20 && echo "ok: the Glass slider is on Appearance" || fail "no glass row"
click_id set-look:glass-number; wtype "25"; wtype -k Return; sleep 1
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if d["glass"]==0.25 else 1)' "$SHAPE" && echo "ok: 25 is written as a quarter of the way to tinted" || fail "glass file: $(cat "$SHAPE" 2>&1)"
echo "--- corners: each kind of thing has its own slider"
SHAPE=$SCRATCH/config/helix/shape-settings.json
for id in look:corner-windows look:corner-helix shape:bar shape:dock shape:panels shape:toasts shape:apps; do wait_id "set-$id-number" 20 && echo "ok: a slider for $id" || fail "no row $id"; done
[ "$(field window_corner_radius)" = "0" ] && [ "$(field helix_window_corner_radius)" = "0" ] && echo "ok: windows start square, as this configuration says" || fail "start: $(ipc '"HelixLook"')"
# (the window rows sit above the fold of this small window, so their logic is tested in look.rs; here the compositor takes both)
r=$(ipc '{"HelixSetLook":{"window_corner_radius":20,"helix_window_corner_radius":2}}'); [ "$r" = '"Handled"' ] && [ "$(field window_corner_radius)" = "20" ] && [ "$(field helix_window_corner_radius)" = "2" ] && echo "ok: the compositor holds one radius for windows and one for Helix apps" || fail "compositor: $(ipc '"HelixLook"')"
ipc '{"HelixSetLook":{"window_corner_radius":8}}' >/dev/null; sleep 0.5
[ "$(field window_corner_radius)" = "8" ] && [ "$(field helix_window_corner_radius)" = "2" ] && echo "ok: changing the windows leaves the Helix apps' radius alone" || fail "after windows: $(ipc '"HelixLook"')"
click_id set-shape:bar-number; wtype "0"; wtype -k Return; sleep 1
click_id set-shape:dock-number; wtype "150"; wtype -k Return; sleep 1
click_id set-shape:panels-number; wtype "50"; wtype -k Return; sleep 1
click_id set-shape:toasts-number; wtype "200"; wtype -k Return; sleep 1
click_id set-shape:apps-number; wtype "25"; wtype -k Return; sleep 1
python3 - "$SHAPE" <<'PY' && echo "ok: every part has its own number, and the glass is kept" || fail "shape file: $(cat "$SHAPE")"
import json, sys
d = json.load(open(sys.argv[1]))
assert (d["bar"], d["dock"], d["panels"], d["toasts"], d["apps"]) == (0.0, 1.5, 0.5, 2.0, 0.25), d
assert d["glass"] == 0.25 and d["windows"] == 12 and d["helix_windows"] is None, d
PY
r=$(ipc '{"HelixSetLook":{"helix_window_corner_radius":99}}'); echo "$r" | grep -qi "0 to 32" && echo "ok: an impossible corner is refused" || fail "no refusal: $r"
python3 - <<'PY'
import json, os, socket
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["NIRI_SOCKET"]); s.send(b'{"Action":{"LoadConfigFile":{}}}\n'); s.settimeout(5); s.recv(4096)
PY
sleep 2
[ "$(field window_corner_radius)" = "8" ] && [ "$(field helix_window_corner_radius)" = "2" ] && echo "ok: both corners survive a reload" || fail "lost on reload: $(ipc '"HelixLook"')"
# Inside Helix apps: with the apps' scale at 0 the page itself is drawn square (looked at in the picture).
echo '{"version":2,"windows":8,"bar":1,"dock":1,"panels":1,"toasts":1,"apps":0,"glass":0.6}' > "$SHAPE"
sleep 3; shot look-square
echo '{"version":2,"windows":8,"bar":1,"dock":1,"panels":1,"toasts":1,"apps":2,"glass":0.6}' > "$SHAPE"
sleep 3; shot look-round
shot look-after
[ "$FAILED" = 0 ] && echo "PASS: settings look" || exit 1
