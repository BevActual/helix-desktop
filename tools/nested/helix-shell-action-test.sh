#!/bin/bash
# helix-shell "<command>": a key bind asks the shell by name. With no shell on the bus the request goes the old way, to helix-panel, with the same
# name; a name that is not a plain short word is never passed on.
#   HELIX_NESTED_SCRIPT=tools/nested/helix-shell-action-test.sh tools/nested/run.sh helix-shell-action tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
sleep 3
BIN=$HOME/.local/bin; LOG=$SCRATCH/helix-panel.log
mkdir -p "$BIN"; printf '#!/bin/sh\necho "$@" >> "%s"\n' "$LOG" > "$BIN/helix-panel"; chmod +x "$BIN/helix-panel"
act() { python3 - "$1" <<'PY'
import json, os, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["NIRI_SOCKET"])
s.send((json.dumps({"Action": {"HelixShell": {"command": sys.argv[1]}}}) + "\n").encode()); s.settimeout(5); print(s.recv(4096).decode().strip())
PY
}
r=$(act lock); echo "$r" | grep -q Ok && echo "ok: the action is accepted" || fail "refused: $r"
for _ in $(seq 1 20); do grep -q "^lock$" "$LOG" 2>/dev/null && break; sleep 0.25; done
grep -q "^lock$" "$LOG" 2>/dev/null && echo "ok: with no shell on the bus it goes to helix-panel by name" || fail "helix-panel not run: $(cat "$LOG" 2>&1)"
act 'lock; touch /tmp/helix-shell-injected' >/dev/null; sleep 1.5
[ ! -e /tmp/helix-shell-injected ] && ! grep -q ";" "$LOG" && echo "ok: a name that is not a plain word is not passed on" || fail "injected: $(cat "$LOG")"
echo "--- binds: reported, overridden, kept over a reload, given back"
req() { python3 - "$1" <<'PY'
import os, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["NIRI_SOCKET"]); s.send(sys.argv[1].encode() + b"\n"); s.settimeout(5); print(s.recv(1 << 20).decode().strip())
PY
}
binds() { req '"HelixBinds"'; }
bind_field() { binds | python3 -c "import json,sys; b=json.load(sys.stdin)['Ok']['HelixBinds']; m=[x for x in b if x['chord']=='$1']; print(m[0]['$2'] if m else '')"; }
[ "$(binds | python3 -c "import json,sys; print(len(json.load(sys.stdin)['Ok']['HelixBinds']))")" -ge 0 ] && echo "ok: the compositor reports its binds (a list)" || fail "no binds: $(binds | head -c 200)"
r=$(req '{"HelixSetBinds":{"set":[{"chord":"Mod+F9","action":"helix-shell \"launcher\""}]}}'); echo "$r" | grep -q Handled && echo "ok: a bind is set" || fail "set: $r"
[ "$(bind_field Mod+F9 action)" = "helix-shell" ] && [ "$(bind_field Mod+F9 source)" = "helix" ] && echo "ok: it is reported as a Helix override" || fail "report: $(binds | head -c 300)"
python3 - <<'PY'
import os, socket
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["NIRI_SOCKET"]); s.send(b'{"Action":{"LoadConfigFile":{}}}\n'); s.settimeout(5); s.recv(4096)
PY
sleep 2
[ "$(bind_field Mod+F9 action)" = "helix-shell" ] && echo "ok: it survives a reload" || fail "lost on reload"
r=$(req '{"HelixSetBinds":{"set":[{"chord":"Mod+F9","action":"focus-column-left; spawn \"x\""}]}}'); echo "$r" | grep -qi "one thing to do" && echo "ok: two actions in one are refused" || fail "no refusal: $r"
r=$(req '{"HelixSetBinds":{"reset":["Mod+F9"]}}'); echo "$r" | grep -q Handled; sleep 2
[ -z "$(bind_field Mod+F9 action)" ] && echo "ok: given back, it is gone" || fail "still there after reset"
[ "$FAILED" = 0 ] && echo "PASS: helix-shell action" || exit 1
