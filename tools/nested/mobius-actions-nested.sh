#!/bin/bash
# Mobius in Tools mode with the desktop service built from this tree running (the actions catalogue lives there), for mobius-actions-test.sh.
# Started before Mobius so the bus never activates an installed service from the real HOME.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
"${HELIX_SERVICE_BIN:-$ROOT/target/fast/helix-desktop}" > "$XDG_STATE_HOME/helix-desktop.log" 2>&1 &
for _ in $(seq 1 40); do busctl --user status org.helix.Desktop >/dev/null 2>&1 && break; sleep 0.25; done
exec "$ROOT/tools/nested/mobius-tools-nested.sh"
