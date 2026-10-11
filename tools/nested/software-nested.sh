#!/bin/bash
# Helix Software with a fake Flatpak catalog (no real installs): the base rows plus filler so the list pages.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_SOFTWARE_BIN:-$(ls -t "$ROOT/target/fast/helix-software" "$ROOT/target/release/helix-software" 2>/dev/null | head -1)}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
python3 - "$ROOT/tools/nested/software-fake.json" "$XDG_STATE_HOME/software-fake.json" <<'PY'
import json, sys
rows = json.load(open(sys.argv[1]))
for i in range(150):
    rows.append({"id": f"org.filler.App{i:03d}", "title": f"Filler {i:03d}", "summary": f"Filler application number {i}", "kind": "flatpak",
                 "installed": False, "update": False, "remote": "flathub", "version": "1.0", "size": 1000000 + i})
json.dump(rows, open(sys.argv[2], "w"))
PY
export HELIX_SOFTWARE_FAKE="$XDG_STATE_HOME/software-fake.json"
export HELIX_SOFTWARE_FAKE_STEP_MS=${HELIX_SOFTWARE_FAKE_STEP_MS:-400}
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
