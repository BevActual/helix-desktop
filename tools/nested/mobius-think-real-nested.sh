#!/bin/bash
# Mobius against the REAL inference service with one model that is not in the library: $HELIX_THINK_MODEL (a .gguf; default
# ~/.cache/helix-experiments/models/Qwen3-4B-Q4_K_M.gguf), linked into a private models folder. Nothing is written outside the scratch dirs.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_MOBIUS_BIN:-$(ls -t "$ROOT/target/fast/helix-mobius" "$ROOT/target/release/helix-mobius" 2>/dev/null | head -1)}
INFERENCE=${HELIX_INFERENCE_BIN:-$(ls /usr/local/bin/helix-inference "$ROOT"/target/fast/helix-inference "$ROOT"/target/release/helix-inference 2>/dev/null | head -1)}
REAL_HOME=$(getent passwd "$(id -u)" | cut -d: -f6)
MODEL=${HELIX_THINK_MODEL:-$REAL_HOME/.cache/helix-experiments/models/Qwen3-4B-Q4_K_M.gguf}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
mkdir -p "$XDG_DATA_HOME/helix/models"
[ -f "$MODEL" ] && ln -sf "$MODEL" "$XDG_DATA_HOME/helix/models/$(basename "$MODEL")"
( HOME=$REAL_HOME XDG_CACHE_HOME=$REAL_HOME/.cache "$INFERENCE" > "$XDG_STATE_HOME/helix-inference.log" 2>&1 ) &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
sleep 2
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
