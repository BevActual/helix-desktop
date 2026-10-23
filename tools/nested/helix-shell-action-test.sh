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
[ "$FAILED" = 0 ] && echo "PASS: helix-shell action" || exit 1
