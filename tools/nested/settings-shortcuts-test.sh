#!/bin/bash
# Settings > Keyboard > shortcuts: the compositor's binds are listed from what it reports, a shortcut is moved by choosing the modifiers and pressing the
# key (the config files are not touched), turned off, and given back. The compositor keeps the change and shows it as a Helix override.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-shortcuts-test.sh tools/nested/run.sh settings-shortcuts tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=binds { Mod+F7 { focus-column-left; }; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
export UI_ORIGIN="12 12" UI_SCREEN="1280 720"
req() { python3 - "$1" <<'PY'
import os, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["NIRI_SOCKET"]); s.send(sys.argv[1].encode() + b"\n"); s.settimeout(5); print(s.recv(1 << 20).decode().strip())
PY
}
bound() { req '"HelixBinds"' | python3 -c "import json,sys; b=json.load(sys.stdin)['Ok']['HelixBinds']; m=[x for x in b if x['chord'].lower()=='$1'.lower()]; print(m[0]['action']+'/'+m[0]['source'] if m else '-')"; }
for _ in $(seq 1 120); do grep -q "SETTINGS bluetooth" "$LOG" && break; sleep 0.25; done; sleep 2
[ "$(bound Mod+F7)" = "focus-column-left/config" ] && echo "ok: the compositor reports the config's bind" || fail "start: $(req '"HelixBinds"' | head -c 300)"
click_id page-keyboard; sleep 1.5
# The shortcuts are below the typing rows; the keyboard scrolls the page (the pointer harness cannot).
for _ in 1 2 3; do has_id shortcut-change:Mod+F7 && break; wtype -k End; sleep 0.6; done
wait_id shortcut-change:Mod+F7 20 && echo "ok: the page lists the shortcut with a Change button" || { shot nolist; fail "no row for Mod+F7"; }
click_id shortcut-change:Mod+F7; sleep 0.8
wait_id shortcut-catcher 10 && echo "ok: Change opens the recorder" || fail "no recorder"
wtype -k F8; sleep 1.5
[ "$(bound Mod+F8)" = "focus-column-left/helix" ] && echo "ok: Super with F8 now does what Super with F7 did" || fail "move: $(req '"HelixBinds"' | head -c 400)"
[ "$(bound Mod+F7)" = "none/helix" ] && echo "ok: and the old keys are off" || fail "old: $(bound Mod+F7)"
grep -q "Mod+F7" "$SCRATCH/config/niri/config.kdl" 2>/dev/null; true
python3 - <<'PY'
import json, os, socket
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["NIRI_SOCKET"]); s.send(b'{"Action":{"LoadConfigFile":{}}}\n'); s.settimeout(5); s.recv(4096)
PY
sleep 2
[ "$(bound Mod+F8)" = "focus-column-left/helix" ] && [ "$(bound Mod+F7)" = "none/helix" ] && echo "ok: it all survives a reload" || fail "reload: $(bound Mod+F8) $(bound Mod+F7)"
shot shortcuts-moved
r=$(req '{"HelixSetBinds":{"reset":["Mod+F8","Mod+F7"]}}'); echo "$r" | grep -q Handled && sleep 2
[ "$(bound Mod+F7)" = "focus-column-left/config" ] && [ "$(bound Mod+F8)" = "-" ] && echo "ok: given back, the config's bind returns and the override is gone" || fail "reset: $(bound Mod+F7) $(bound Mod+F8)"
[ "$FAILED" = 0 ] && echo "PASS: settings shortcuts" || exit 1
