#!/bin/bash
# Started by run.sh: a fake greetd on a private socket, then the real helix-greeter pointed at it.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_GREETER_BIN:-$(ls -t "$ROOT/target/fast/helix-greeter" "$ROOT/target/release/helix-greeter" 2>/dev/null | head -1)}
export GREETD_SOCK="$XDG_STATE_HOME/greetd.sock"
python3 "$ROOT/tools/nested/fake-greetd.py" "$GREETD_SOCK" "$XDG_STATE_HOME/greetd-log.jsonl" &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
for _ in $(seq 1 40); do [ -S "$GREETD_SOCK" ] && break; sleep 0.1; done
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
