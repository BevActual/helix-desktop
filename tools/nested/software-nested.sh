#!/bin/bash
# Helix Software with a fake Flatpak catalog (no real installs): the base rows plus filler so the list pages.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_SOFTWARE_BIN:-$(ls -t "$ROOT/target/fast/helix-software" "$ROOT/target/release/helix-software" 2>/dev/null | head -1)}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
python3 - "$ROOT/tools/nested/software-fake.json" "$XDG_STATE_HOME/software-fake.json" <<'PY'
import json, sys
rows = json.load(open(sys.argv[1]))
# Enough extra applications in one category that its page needs a second window of rows.
for i in range(100):
    rows.append({"id": f"org.extra.Game{i:03d}", "title": f"Extra Game {i:03d}", "summary": f"Another game number {i}", "kind": "flatpak",
                 "installed": False, "update": False, "remote": "flathub", "version": "1.0", "size": 2000000 + i,
                 "categories": ["Game"], "released": 1760000000 + i, "verified": False})
json.dump(rows, open(sys.argv[2], "w"))
PY
export HELIX_SOFTWARE_FAKE="$XDG_STATE_HOME/software-fake.json"
export HELIX_SOFTWARE_FAKE_DAY=${HELIX_SOFTWARE_FAKE_DAY:-1}
export HELIX_SOFTWARE_FAKE_STEP_MS=${HELIX_SOFTWARE_FAKE_STEP_MS:-1200}
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
