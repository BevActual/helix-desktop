#!/bin/bash
# Helix System Monitor with fake /proc and tools/nested/fake-desktop.py (the settings service), for the interface scale.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
python3 "$ROOT/tools/nested/fake-desktop.py" "$XDG_STATE_HOME" > "$XDG_STATE_HOME/fake-desktop.log" 2>&1 &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
for _ in $(seq 1 50); do grep -q "ready" "$XDG_STATE_HOME/fake-desktop.log" 2>/dev/null && break; sleep 0.1; done
exec "$ROOT/tools/nested/system-monitor-nested.sh"
