#!/bin/bash
# Mobius against tools/nested/fake-shell-act.py (a scripted assistant session), for the task card. No inference service is needed:
# only the hand-over and what comes back are exercised.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_MOBIUS_BIN:-$(ls -t "$ROOT/target/fast/helix-mobius" "$ROOT/target/release/helix-mobius" 2>/dev/null | head -1)}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
python3 "$ROOT/tools/nested/fake-shell-act.py" "$XDG_STATE_HOME" > "$XDG_STATE_HOME/fake-shell.log" 2>&1 &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
for _ in $(seq 1 50); do grep -q "ready" "$XDG_STATE_HOME/fake-shell.log" 2>/dev/null && break; sleep 0.1; done
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
