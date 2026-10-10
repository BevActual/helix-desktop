#!/bin/bash
# Helix Software with a fake Flatpak catalog (no real installs).
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_SOFTWARE_BIN:-$(ls -t "$ROOT/target/fast/helix-software" "$ROOT/target/release/helix-software" 2>/dev/null | head -1)}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
export HELIX_SOFTWARE_FAKE="$ROOT/tools/nested/software-fake.json"
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
