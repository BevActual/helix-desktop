#!/bin/bash
# Started by run.sh: the real helix-service (told that the Settings binary under test is the one that may manage agents), two applications
# that already hold grants, a little audit history, then Helix Settings itself.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_SETTINGS_BIN:-$(ls -t "$ROOT/rust/target/fast/helix-settings" "$ROOT/rust/target/release/helix-settings" 2>/dev/null | head -1)}
export PIPEWIRE_REMOTE=helix-nested-no-audio HELIX_RISK_POLICY="$ROOT/protocols/ipc/msg-risk.toml" HELIX_MANAGER_EXE="$BIN"
mkdir -p "$XDG_STATE_HOME/noctalia/agent"
printf '{"clients":{"exe:/opt/agents/helper.py":["observe","act"],"exe:/opt/agents/notes-agent":["observe"]},"version":1}' > "$XDG_STATE_HOME/noctalia/agent/grants.json"
chmod 600 "$XDG_STATE_HOME/noctalia/agent/grants.json"
cat > "$XDG_STATE_HOME/noctalia/agent/audit.jsonl" <<'AUDIT'
{"time":"2026-10-07T10:00:00Z","client":"exe:/opt/agents/helper.py","tool":"volume","args":["set","30"],"tier":"act","decision":"allowed","outcome":"ok","tainted":false}
{"time":"2026-10-07T10:00:05Z","client":"exe:/opt/agents/notes-agent","tool":"app-launch","args":["Browser"],"tier":"confirm","decision":"denied","outcome":"the user declined","tainted":true}
AUDIT
chmod 600 "$XDG_STATE_HOME/noctalia/agent/audit.jsonl"
"$ROOT/build-debug/helix-service" > "$XDG_STATE_HOME/helix-service.log" 2>&1 &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
