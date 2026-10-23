#!/bin/bash
# Settings > Multitasking: each change is sent to the Helix compositor, takes effect there at once (asked back over IPC), is remembered in
# look-overrides.json and survives a reload of the configuration.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-multitask-test.sh tools/nested/run.sh settings-multitask tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
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
click_id page-multitask; sleep 1.5
wait_id set-mt:focus 20 && has_id set-mt:center-always && has_id set-mt:corner && has_id set-mt:back && echo "ok: the multitasking rows are on the page" || fail "rows missing"
[ "$(field focus_follows_mouse)" = "False" ] && [ "$(field hot_corner)" = "True" ] && [ "$(field center_focused)" = "never" ] && echo "ok: they start as the compositor reports" || fail "start: $(ipc '"HelixLook"')"
shot multitask; sleep 1; shot multitask
click_id set-mt:focus; sleep 1.2
[ "$(field focus_follows_mouse)" = "True" ] && echo "ok: focus follows the pointer took effect" || fail "ffm: $(ipc '"HelixLook"')"
click_id set-mt:corner; sleep 1.2
[ "$(field hot_corner)" = "False" ] && echo "ok: the hot corner is off" || fail "corner: $(field hot_corner)"
click_id set-mt:back; sleep 1.2
[ "$(field workspace_back_and_forth)" = "True" ] && echo "ok: back-and-forth is on (it starts off)" || fail "back: $(field workspace_back_and_forth)"
# (centring moves the Settings window itself, so it is chosen last)
click_id set-mt:center-always; sleep 1.2
[ "$(field center_focused)" = "always" ] && echo "ok: centring is always" || fail "center: $(field center_focused)"
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if d["focus_follows_mouse"] is True and d["hot_corner"] is False and d["center_focused"]=="always" and d["workspace_back_and_forth"] is True else 1)' "$OVR" && echo "ok: all of it is remembered in look-overrides.json" || fail "overrides: $(cat "$OVR" 2>&1)"
python3 - <<'PY'
import json, os, socket
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["NIRI_SOCKET"]); s.send(b'{"Action":{"LoadConfigFile":{}}}\n'); s.settimeout(5); s.recv(4096)
PY
sleep 2
[ "$(field focus_follows_mouse)" = "True" ] && [ "$(field center_focused)" = "always" ] && [ "$(field hot_corner)" = "False" ] && echo "ok: after a reload they are still in effect" || fail "lost on reload: $(ipc '"HelixLook"')"
echo "--- per-application rules"
r=$(ipc '{"HelixSetLook":{"app_rules":[{"app_id":"org.gnome.Nautilus","open":"floating","private":true,"opacity":80}]}}'); [ "$r" = '"Handled"' ] && echo "ok: the compositor takes an application rule" || fail "rule: $r"
python3 -c 'import json,sys; d=json.loads(sys.argv[1]); r=d["app_rules"]; sys.exit(0 if len(r)==1 and r[0]["app_id"]=="org.gnome.Nautilus" and r[0]["open"]=="floating" and r[0]["private"] is True and r[0]["opacity"]==80 else 1)' "$(ipc '"HelixLook"')" && echo "ok: and reads it back" || fail "read back: $(ipc '"HelixLook"')"
r=$(ipc '{"HelixSetLook":{"app_rules":[{"app_id":".*"}]}}'); echo "$r" | grep -qi "letters, digits" && echo "ok: a pattern in place of a name is refused" || fail "no refusal: $r"
python3 - <<'PY'
import json, os, socket
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["NIRI_SOCKET"]); s.send(b'{"Action":{"LoadConfigFile":{}}}\n'); s.settimeout(5); s.recv(4096)
PY
sleep 2
python3 -c 'import json,sys; d=json.loads(sys.argv[1]); sys.exit(0 if len(d["app_rules"])==1 else 1)' "$(ipc '"HelixLook"')" && echo "ok: the rule survives a reload, and is not doubled" || fail "after reload: $(ipc '"HelixLook"')"
# (the application rows are unit-tested in look.rs: centring moved this window, so it is not clicked again here)
shot multitask-after
[ "$FAILED" = 0 ] && echo "PASS: settings multitask" || exit 1
