#!/bin/bash
# Mobius against this machine's REAL inference service and models (heavy: loads a GGUF). Nothing is written outside the scratch chats.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_MOBIUS_BIN:-$(ls -t "$ROOT/target/fast/helix-mobius" "$ROOT/target/release/helix-mobius" 2>/dev/null | head -1)}
INFERENCE=${HELIX_INFERENCE_BIN:-$(ls /usr/local/bin/helix-inference "$ROOT"/target/fast/helix-inference "$ROOT"/target/release/helix-inference 2>/dev/null | head -1)}
REAL_HOME=$(getent passwd "$(id -u)" | cut -d: -f6)
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
# A copy of this machine's routing (read only, never written back), so the picker shows the person's real setup: MOBIUS_REAL_ROUTING=1.
if [ -n "${MOBIUS_REAL_ROUTING:-}" ] && [ -f "$REAL_HOME/.local/state/helix/inference-routing.toml" ]; then
  mkdir -p "$XDG_STATE_HOME/helix"; cp "$REAL_HOME/.local/state/helix/inference-routing.toml" "$XDG_STATE_HOME/helix/"
fi
# The inference service reads models from the real data directory; everything else stays private.
( HOME=$REAL_HOME XDG_DATA_HOME=$REAL_HOME/.local/share XDG_CACHE_HOME=$REAL_HOME/.cache "$INFERENCE" > "$XDG_STATE_HOME/helix-inference.log" 2>&1 ) &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
sleep 2
# Mobius reads the models folder itself (for its library and picker), so it is pointed at the real one the service uses.
export HELIX_MODEL_DIR="$REAL_HOME/.local/share/helix/models"
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
