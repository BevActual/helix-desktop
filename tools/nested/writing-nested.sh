#!/bin/bash
# The Tauri shell with a scripted model service (its reply is HELIX_FAKE_REPLY), so Writing Tools never reach a real model, then the
# real desktop service and shell as tauri-shell.sh starts them.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
INFERENCE=${HELIX_INFERENCE_BIN:-$(ls -t "$ROOT/target/fast/helix-inference" "$ROOT/target/release/helix-inference" 2>/dev/null | head -1)}
HELIX_INFERENCE_BACKEND=fake HELIX_ENGINE_SANDBOX=off "$INFERENCE" > "$XDG_STATE_HOME/helix-inference.log" 2>&1 &
for _ in $(seq 1 60); do busctl --user status org.helix.Inference >/dev/null 2>&1 && break; sleep 0.25; done
exec "$ROOT/tools/nested/tauri-shell.sh"
