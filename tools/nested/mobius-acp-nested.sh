#!/bin/bash
# Mobius with a pretend coding agent (core/helix-acp/tests/fake_agent.py) listed, trusted, and a folder picker that chooses ~/project.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$XDG_STATE_HOME/pick-bin" "$HOME/project" "$XDG_DATA_HOME/helix/mobius"
printf 'alpha\nbeta\n' > "$HOME/project/notes.txt"
cat > "$XDG_STATE_HOME/pick-bin/zenity" <<'PICK'
#!/bin/bash
printf '%s/project\n' "$HOME"
PICK
chmod +x "$XDG_STATE_HOME/pick-bin/zenity"
python3 - "$XDG_DATA_HOME/helix/mobius/acp-agents.json" "$ROOT/core/helix-acp/tests/fake_agent.py" <<'PY'
import json, sys
agent = {"id": "custom-fake-agent", "title": "Fake agent", "command": "python3", "args": [sys.argv[2]], "custom": True}
json.dump({"custom": [agent], "trusted": ["custom-fake-agent"]}, open(sys.argv[1], "w"))
PY
export PATH="$XDG_STATE_HOME/pick-bin:$PATH" FAKE=mobius
exec "$ROOT/tools/nested/mobius-nested.sh"
