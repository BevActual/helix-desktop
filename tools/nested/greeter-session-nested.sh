#!/bin/bash
# Started by run.sh: the greeter session script (the compositor it makes, then the greeter) against a fake greetd.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export HELIX_GREETER_BIN=${HELIX_GREETER_BIN:-$(ls -t "$ROOT/target/fast/helix-greeter" "$ROOT/target/release/helix-greeter" 2>/dev/null | head -1)}
export HELIX_GREETER_NIRI=/usr/bin/niri
export HELIX_GREETER_NIRI_CONFIG="$ROOT/greeter/session/greeter-niri.kdl"
export HELIX_GREETER_NESTED=1
export HELIX_GREETER_LOG="$XDG_STATE_HOME/greeter-session.log"
export GREETD_SOCK="$XDG_STATE_HOME/greetd.sock"
python3 "$ROOT/tools/nested/fake-greetd.py" "$GREETD_SOCK" "$XDG_STATE_HOME/greetd-log.jsonl" &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
for _ in $(seq 1 40); do [ -S "$GREETD_SOCK" ] && break; sleep 0.1; done
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$ROOT/greeter/session/helix-greeter-session"
