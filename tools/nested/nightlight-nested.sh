#!/bin/bash
# Started by run.sh: helix-nightlight with the screen set to be warm all the time.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$XDG_CONFIG_HOME/helix"
echo '{"version":1,"enabled":true,"warmth":3000,"scheduled":false,"start":20,"end":7}' > "$XDG_CONFIG_HOME/helix/nightlight.json"
BIN=$(ls -t "$ROOT/target/fast/helix-nightlight" "$ROOT/target/release/helix-nightlight" 2>/dev/null | head -1)
exec "$BIN"
